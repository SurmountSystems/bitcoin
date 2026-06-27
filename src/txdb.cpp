// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txdb.h>

#include <coins.h>
#include <compress/zstd.h>
#include <dbwrapper.h>
#include <logging.h>
#include <primitives/transaction.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>
#include <util/vector.h>

#include <cassert>
#include <cstdlib>
#include <iterator>
#include <utility>

static constexpr uint8_t DB_COIN{'C'};
static constexpr uint8_t DB_BEST_BLOCK{'B'};
static constexpr uint8_t DB_HEAD_BLOCKS{'H'};
// Keys used in previous version that might still be found in the DB:
static constexpr uint8_t DB_COINS{'c'};

namespace {
static constexpr uint8_t COIN_VALUE_VERSION{1};
static constexpr uint8_t COIN_VALUE_UNCOMPRESSED{0};
static constexpr uint8_t COIN_VALUE_COMPRESSED{1};
//! Upper bound for a single decompressed UTXO entry (generous DoS limit).
static constexpr size_t MAX_COIN_VALUE_SIZE{1 << 20};

struct CoinDBValue {
    std::vector<uint8_t> bytes;

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        if (!bytes.empty()) s.write(MakeByteSpan(bytes));
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        bytes.resize(s.size());
        if (!bytes.empty()) s.read(MakeWritableByteSpan(bytes));
    }
};

bool TryDeserializeCoin(std::span<const uint8_t> data, Coin& coin)
{
    if (data.empty()) return false;
    try {
        DataStream ss{data};
        ss >> coin;
    } catch (const std::exception&) {
        return false;
    }
    return !coin.IsSpent();
}

bool DecodeCoinValue(std::span<const uint8_t> data, const compress::UtxoZstd& zstd, Coin& coin)
{
    if (TryDeserializeCoin(data, coin)) return true;

    if (data.size() >= 2 && data[0] == COIN_VALUE_VERSION && data[1] == COIN_VALUE_UNCOMPRESSED) {
        return TryDeserializeCoin(data.subspan(2), coin);
    }

    if (data.size() >= 2 && data[0] == COIN_VALUE_VERSION && data[1] == COIN_VALUE_COMPRESSED) {
        if (!zstd) return false;
        std::vector<uint8_t> decompressed;
        if (!zstd.Decompress(data.subspan(2), decompressed, MAX_COIN_VALUE_SIZE)) return false;
        return TryDeserializeCoin(decompressed, coin);
    }

    return false;
}

std::vector<uint8_t> SerializeCoin(const Coin& coin)
{
    std::vector<uint8_t> serialized;
    VectorWriter{serialized, 0, coin};
    return serialized;
}

std::vector<uint8_t> EncodeCoinValue(const Coin& coin, const CoinsViewOptions& options, const compress::UtxoZstd& zstd)
{
    const std::vector<uint8_t> serialized{SerializeCoin(coin)};
    if (!options.utxo_zstd || !zstd) {
        return serialized;
    }

    std::vector<uint8_t> compressed;
    if (!zstd.Compress(serialized, compressed, options.utxo_zstd_level)) {
        return serialized;
    }

    const size_t stored_size{2 + compressed.size()};
    if (stored_size >= serialized.size()) {
        LogDebug(BCLog::COINDB, "Skipping UTXO zstd compression: stored size %u >= legacy size %u\n",
                 stored_size, serialized.size());
        return serialized;
    }

    std::vector<uint8_t> stored;
    stored.reserve(stored_size);
    stored.push_back(COIN_VALUE_VERSION);
    stored.push_back(COIN_VALUE_COMPRESSED);
    stored.insert(stored.end(), compressed.begin(), compressed.end());
    return stored;
}

struct CoinEntry {
    COutPoint* outpoint;
    uint8_t key;
    explicit CoinEntry(const COutPoint* ptr) : outpoint(const_cast<COutPoint*>(ptr)), key(DB_COIN)  {}

    SERIALIZE_METHODS(CoinEntry, obj) { READWRITE(obj.key, obj.outpoint->hash, VARINT(obj.outpoint->n)); }
};

compress::UtxoZstd LoadUtxoZstd(const CoinsViewOptions& options)
{
    fs::path dict_path;
    if (options.utxo_zstd_dict_path) {
        dict_path = *options.utxo_zstd_dict_path;
    } else {
        dict_path = compress::DefaultUtxoDictionaryPath();
    }

    if (dict_path.empty()) return {};

    if (auto dictionary{compress::LoadDictionaryFile(dict_path)}) {
        return compress::UtxoZstd{std::move(*dictionary)};
    }
    return {};
}

} // namespace

bool CCoinsViewDB::NeedsUpgrade()
{
    std::unique_ptr<CDBIterator> cursor{m_db->NewIterator()};
    // DB_COINS was deprecated in v0.15.0, commit
    // 1088b02f0ccd7358d2b7076bb9e122d59d502d02
    cursor->Seek(std::make_pair(DB_COINS, uint256{}));
    return cursor->Valid();
}

CCoinsViewDB::CCoinsViewDB(DBParams db_params, CoinsViewOptions options) :
    m_db_params{std::move(db_params)},
    m_options{std::move(options)},
    m_db{std::make_unique<CDBWrapper>(m_db_params)},
    m_utxo_zstd{LoadUtxoZstd(m_options)}
{
    if (m_options.utxo_zstd && !m_utxo_zstd) {
        LogWarning("UTXO zstd compression is enabled (-utxozstd=1) but no dictionary is loaded; "
                   "new UTXO writes will be stored uncompressed.\n");
    }
}

void CCoinsViewDB::ResizeCache(size_t new_cache_size)
{
    // We can't do this operation with an in-memory DB since we'll lose all the coins upon
    // reset.
    if (!m_db_params.memory_only) {
        // Reopen the LMDB environment to tune reader pool size (maxreaders) and refresh
        // map-size tracking from the on-disk database. The existing map size is preserved.
        m_db.reset();
        m_db_params.cache_bytes = new_cache_size;
        m_db_params.wipe_data = false;
        m_db = std::make_unique<CDBWrapper>(m_db_params);
    }
}

bool CCoinsViewDB::ReadCoinValue(const COutPoint& outpoint, Coin& coin) const
{
    CoinDBValue stored;
    if (!m_db->Read(CoinEntry(&outpoint), stored)) return false;
    return DecodeCoinValue(stored.bytes, m_utxo_zstd, coin);
}

void CCoinsViewDB::WriteCoinValue(CDBBatch& batch, const COutPoint& outpoint, const Coin& coin)
{
    CoinDBValue stored;
    stored.bytes = EncodeCoinValue(coin, m_options, m_utxo_zstd);
    batch.Write(CoinEntry(&outpoint), stored);
}

std::optional<Coin> CCoinsViewDB::GetCoin(const COutPoint& outpoint) const
{
    if (Coin coin; ReadCoinValue(outpoint, coin)) return coin;
    return std::nullopt;
}

bool CCoinsViewDB::HaveCoin(const COutPoint &outpoint) const {
    return m_db->Exists(CoinEntry(&outpoint));
}

uint256 CCoinsViewDB::GetBestBlock() const {
    uint256 hashBestChain;
    if (!m_db->Read(DB_BEST_BLOCK, hashBestChain))
        return uint256();
    return hashBestChain;
}

std::vector<uint256> CCoinsViewDB::GetHeadBlocks() const {
    std::vector<uint256> vhashHeadBlocks;
    if (!m_db->Read(DB_HEAD_BLOCKS, vhashHeadBlocks)) {
        return std::vector<uint256>();
    }
    return vhashHeadBlocks;
}

bool CCoinsViewDB::BatchWrite(CoinsViewCacheCursor& cursor, const uint256 &hashBlock) {
    CDBBatch batch(*m_db);
    size_t count = 0;
    size_t changed = 0;
    assert(!hashBlock.IsNull());

    uint256 old_tip = GetBestBlock();
    if (old_tip.IsNull()) {
        // We may be in the middle of replaying.
        std::vector<uint256> old_heads = GetHeadBlocks();
        if (old_heads.size() == 2) {
            if (old_heads[0] != hashBlock) {
                LogPrintLevel(BCLog::COINDB, BCLog::Level::Error, "The coins database detected an inconsistent state, likely due to a previous crash or shutdown. You will need to restart bitcoind with the -reindex-chainstate or -reindex configuration option.\n");
            }
            assert(old_heads[0] == hashBlock);
            old_tip = old_heads[1];
        }
    }

    // In the first batch, mark the database as being in the middle of a
    // transition from old_tip to hashBlock.
    // A vector is used for future extensibility, as we may want to support
    // interrupting after partial writes from multiple independent reorgs.
    batch.Erase(DB_BEST_BLOCK);
    batch.Write(DB_HEAD_BLOCKS, Vector(hashBlock, old_tip));

    for (auto it{cursor.Begin()}; it != cursor.End();) {
        if (it->second.IsDirty()) {
            CoinEntry entry(&it->first);
            if (it->second.coin.IsSpent())
                batch.Erase(entry);
            else
                WriteCoinValue(batch, it->first, it->second.coin);
            changed++;
        }
        count++;
        it = cursor.NextAndMaybeErase(*it);
        if (batch.SizeEstimate() > m_options.batch_write_bytes) {
            LogDebug(BCLog::COINDB, "Writing partial batch of %.2f MiB\n", batch.SizeEstimate() * (1.0 / 1048576.0));
            m_db->WriteBatch(batch);
            batch.Clear();
            if (m_options.simulate_crash_ratio) {
                static FastRandomContext rng;
                if (rng.randrange(m_options.simulate_crash_ratio) == 0) {
                    LogError("Simulating a crash. Goodbye.");
                    _Exit(0);
                }
            }
        }
    }

    // In the last batch, mark the database as consistent with hashBlock again.
    batch.Erase(DB_HEAD_BLOCKS);
    batch.Write(DB_BEST_BLOCK, hashBlock);

    LogDebug(BCLog::COINDB, "Writing final batch of %.2f MiB\n", batch.SizeEstimate() * (1.0 / 1048576.0));
    bool ret = m_db->WriteBatch(batch);
    LogDebug(BCLog::COINDB, "Committed %u changed transaction outputs (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    return ret;
}

size_t CCoinsViewDB::EstimateSize() const
{
    return m_db->EstimateSize(DB_COIN, uint8_t(DB_COIN + 1));
}

/** Specialization of CCoinsViewCursor to iterate over a CCoinsViewDB */
class CCoinsViewDBCursor: public CCoinsViewCursor
{
public:
    // Prefer using CCoinsViewDB::Cursor() since we want to perform some
    // cache warmup on instantiation.
    CCoinsViewDBCursor(CDBIterator* pcursorIn, const uint256&hashBlockIn, const compress::UtxoZstd& zstd_in):
        CCoinsViewCursor(hashBlockIn), pcursor(pcursorIn), zstd(zstd_in) {}
    ~CCoinsViewDBCursor() = default;

    bool GetKey(COutPoint &key) const override;
    bool GetValue(Coin &coin) const override;

    bool Valid() const override;
    void Next() override;

private:
    std::unique_ptr<CDBIterator> pcursor;
    std::pair<char, COutPoint> keyTmp;
    const compress::UtxoZstd& zstd;

    friend class CCoinsViewDB;
};

std::unique_ptr<CCoinsViewCursor> CCoinsViewDB::Cursor() const
{
    auto i = std::make_unique<CCoinsViewDBCursor>(
        const_cast<CDBWrapper&>(*m_db).NewIterator(), GetBestBlock(), m_utxo_zstd);
    /* It seems that there are no "const iterators" for the database wrapper.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    i->pcursor->Seek(DB_COIN);
    // Cache key of first record
    if (i->pcursor->Valid()) {
        CoinEntry entry(&i->keyTmp.second);
        i->pcursor->GetKey(entry);
        i->keyTmp.first = entry.key;
    } else {
        i->keyTmp.first = 0; // Make sure Valid() and GetKey() return false
    }
    return i;
}

bool CCoinsViewDBCursor::GetKey(COutPoint &key) const
{
    // Return cached key
    if (keyTmp.first == DB_COIN) {
        key = keyTmp.second;
        return true;
    }
    return false;
}

bool CCoinsViewDBCursor::GetValue(Coin &coin) const
{
    CoinDBValue stored;
    if (!pcursor->GetValue(stored)) return false;
    return DecodeCoinValue(stored.bytes, zstd, coin);
}

bool CCoinsViewDBCursor::Valid() const
{
    return keyTmp.first == DB_COIN;
}

void CCoinsViewDBCursor::Next()
{
    pcursor->Next();
    CoinEntry entry(&keyTmp.second);
    if (!pcursor->Valid() || !pcursor->GetKey(entry)) {
        keyTmp.first = 0; // Invalidate cached key after last record so that Valid() and GetKey() return false
    } else {
        keyTmp.first = entry.key;
    }
}
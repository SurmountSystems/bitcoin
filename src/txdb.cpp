// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txdb.h>

#include <coins.h>
#include <chainparams.h>
#include <compress/dict_bootstrap.h>
#include <compress/dict_classify.h>
#include <compress/zstd.h>
#include <dbwrapper.h>
#include <logging.h>
#include <logging/timer.h>
#include <primitives/transaction.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>
#include <util/benchstats.h>
#include <util/time.h>
#include <util/vector.h>

#include <algorithm>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <thread>
#include <utility>

static constexpr uint8_t DB_COIN{'C'};
static constexpr uint8_t DB_BEST_BLOCK{'B'};
static constexpr uint8_t DB_HEAD_BLOCKS{'H'};
// Keys used in previous version that might still be found in the DB:
static constexpr uint8_t DB_COINS{'c'};

namespace {

constexpr auto COIN_PREFETCH_COOLDOWN{60s};

std::atomic<int64_t> g_coin_prefetch_cooldown_until_ticks{0};
std::function<void()> g_coin_prefetch_worker_gate;

bool CoinPrefetchInCooldown()
{
    const int64_t until{g_coin_prefetch_cooldown_until_ticks.load(std::memory_order_relaxed)};
    if (until == 0) return false;
    return SteadyClock::now().time_since_epoch().count() < until;
}

void SetCoinPrefetchCooldown()
{
    g_coin_prefetch_cooldown_until_ticks.store(
        (SteadyClock::now() + COIN_PREFETCH_COOLDOWN).time_since_epoch().count(),
        std::memory_order_relaxed);
}

void ClearCoinPrefetchCooldown()
{
    g_coin_prefetch_cooldown_until_ticks.store(0, std::memory_order_relaxed);
}

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

} // namespace

static constexpr uint8_t COIN_VALUE_VERSION{1};
static constexpr uint8_t COIN_VALUE_UNCOMPRESSED{0};
static constexpr uint8_t COIN_VALUE_COMPRESSED{1};
//! Upper bound for a single decompressed UTXO entry (generous DoS limit).
static constexpr size_t MAX_COIN_VALUE_SIZE{1 << 20};

bool DecodeCoinValue(std::span<const uint8_t> data, const compress::UtxoZstd& zstd, Coin& coin)
{
    if (data.size() >= 2 && data[0] == COIN_VALUE_VERSION && data[1] == COIN_VALUE_UNCOMPRESSED) {
        return TryDeserializeCoin(data.subspan(2), coin);
    }

    if (data.size() >= 2 && data[0] == COIN_VALUE_VERSION && data[1] == COIN_VALUE_COMPRESSED) {
        if (!zstd) return TryDeserializeCoin(data, coin);
        std::vector<uint8_t> decompressed;
        if (!zstd.Decompress(data.subspan(2), decompressed, MAX_COIN_VALUE_SIZE)) {
            // Legacy serialized coins can begin with 0x01 0x01 (version + compressed marker).
            return TryDeserializeCoin(data, coin);
        }
        return TryDeserializeCoin(decompressed, coin);
    }

    if (data.size() >= 2 && data[0] == COIN_VALUE_VERSION && (data[1] & compress::COIN_VALUE_TYPED_BUCKET) != 0) {
        const auto payload{data.subspan(2)};
        const uint8_t bucket_id{static_cast<uint8_t>(data[1] & ~compress::COIN_VALUE_TYPED_BUCKET)};
        const compress::DictZstd* dict{&zstd};
        if (compress::g_dict_bootstrap && compress::g_dict_bootstrap->UseTypedUtxoFormat()
            && bucket_id < compress::NUM_UTXO_BUCKETS) {
            const compress::DictZstd& typed{
                compress::g_dict_bootstrap->UtxoDict(static_cast<compress::UtxoBucket>(bucket_id))};
            if (typed) {
                dict = &typed;
            } else if (bucket_id != 0) {
                LogPrintf("Warning: missing typed UTXO dictionary for bucket %s; falling back to monolithic dictionary\n",
                          compress::UtxoBucketName(static_cast<compress::UtxoBucket>(bucket_id)));
            }
        }
        if (!*dict) {
            return TryDeserializeCoin(payload, coin);
        }
        std::vector<uint8_t> decompressed;
        if (dict->Decompress(payload, decompressed, MAX_COIN_VALUE_SIZE)) {
            return TryDeserializeCoin(decompressed, coin);
        }
        return TryDeserializeCoin(payload, coin);
    }

    if (TryDeserializeCoin(data, coin)) return true;

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
    if (compress::g_dict_bootstrap && compress::g_dict_bootstrap->UseTypedUtxoFormat()) {
        const compress::UtxoBucket bucket{compress::ClassifyCoin(coin, Params())};
        if (compress::g_dict_bootstrap->IsPass1InProgress()) {
            compress::g_dict_bootstrap->OnCoinEncoded(coin, serialized);
            std::vector<uint8_t> stored;
            stored.reserve(2 + serialized.size());
            stored.push_back(COIN_VALUE_VERSION);
            stored.push_back(compress::g_dict_bootstrap->UtxoTypeByteForBucket(bucket));
            stored.insert(stored.end(), serialized.begin(), serialized.end());
            return stored;
        }
        if (compress::g_dict_bootstrap->ShouldCompressOnWrite()) {
            const compress::DictZstd& dict{compress::g_dict_bootstrap->UtxoDict(bucket)};
            std::vector<uint8_t> compressed;
            if (dict.Compress(serialized, compressed, options.utxo_zstd_level)) {
                const size_t stored_size{2 + compressed.size()};
                if (stored_size < serialized.size()) {
                    std::vector<uint8_t> stored;
                    stored.reserve(stored_size);
                    stored.push_back(COIN_VALUE_VERSION);
                    stored.push_back(compress::g_dict_bootstrap->UtxoTypeByteForBucket(bucket));
                    stored.insert(stored.end(), compressed.begin(), compressed.end());
                    compress::g_dict_bootstrap->OnCoinStored(bucket, serialized.size(), stored.size());
                    return stored;
                }
            } else if (!dict && zstd.Compress(serialized, compressed, options.utxo_zstd_level)) {
                const size_t stored_size{2 + compressed.size()};
                if (stored_size < serialized.size()) {
                    LogPrintf("Warning: missing typed UTXO dictionary for bucket %s; falling back to monolithic dictionary\n",
                              compress::UtxoBucketName(bucket));
                    std::vector<uint8_t> stored;
                    stored.reserve(stored_size);
                    stored.push_back(COIN_VALUE_VERSION);
                    stored.push_back(COIN_VALUE_COMPRESSED);
                    stored.insert(stored.end(), compressed.begin(), compressed.end());
                    compress::g_dict_bootstrap->OnCoinStored(bucket, serialized.size(), stored.size());
                    return stored;
                }
            } else if (!dict) {
                LogPrintf("Warning: missing typed UTXO dictionary for bucket %s; writing uncompressed payload\n",
                          compress::UtxoBucketName(bucket));
            }
            std::vector<uint8_t> stored;
            stored.reserve(2 + serialized.size());
            stored.push_back(COIN_VALUE_VERSION);
            stored.push_back(compress::g_dict_bootstrap->UtxoTypeByteForBucket(bucket));
            stored.insert(stored.end(), serialized.begin(), serialized.end());
            compress::g_dict_bootstrap->OnCoinStored(bucket, serialized.size(), stored.size());
            return stored;
        }
    }
    if (!options.utxo_zstd || !zstd || (compress::g_dict_bootstrap && !compress::g_dict_bootstrap->ShouldCompressOnWrite())) {
        return serialized;
    }

    std::vector<uint8_t> compressed;
    if (!zstd.Compress(serialized, compressed, options.utxo_zstd_level)) {
        return serialized;
    }

    const size_t stored_size{2 + compressed.size()};
    if (stored_size >= serialized.size()) {
        LogPrintLevel(BCLog::COINDB, BCLog::Level::Trace,
                      "Skipping UTXO zstd compression: stored size %u >= legacy size %u\n",
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

namespace {

struct CoinEntry {
    COutPoint* outpoint;
    uint8_t key;
    explicit CoinEntry(const COutPoint* ptr) : outpoint(const_cast<COutPoint*>(ptr)), key(DB_COIN)  {}

    SERIALIZE_METHODS(CoinEntry, obj) { READWRITE(obj.key, obj.outpoint->hash, VARINT(obj.outpoint->n)); }
};

void ParallelEncodeCoinValues(std::vector<PendingCoinWrite>& pending,
                              const CoinsViewOptions& options,
                              const compress::UtxoZstd& zstd,
                              int num_workers)
{
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_workers));
    for (int worker = 0; worker < num_workers; ++worker) {
        threads.emplace_back([&]() {
            while (true) {
                const size_t index{next.fetch_add(1, std::memory_order_relaxed)};
                if (index >= pending.size()) break;
                PendingCoinWrite& entry{pending[index]};
                if (!entry.spent) {
                    entry.encoded = EncodeCoinValue(entry.coin, options, zstd);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

bool ParallelPrefetchCoinsImpl(CCoinsViewDB& db,
                               const std::vector<COutPoint>& prevouts,
                               std::vector<PrefetchedCoin>& out,
                               const int num_workers,
                               const compress::UtxoZstd& zstd)
{
    out.clear();
    out.reserve(prevouts.size());
    std::vector<std::optional<Coin>> results(prevouts.size());
    std::atomic<size_t> next{0};
    std::atomic<bool> readers_full{false};
    std::atomic<bool> readers_full_logged{false};
    std::atomic<uint64_t> lmdb_us{0};
    std::atomic<uint64_t> decode_us{0};
    const bool bench{util::g_benchstats_enabled.load(std::memory_order_relaxed)};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_workers));
    for (int worker = 0; worker < num_workers; ++worker) {
        threads.emplace_back([&, first_read = true]() mutable {
            while (true) {
                const size_t index{next.fetch_add(1, std::memory_order_relaxed)};
                if (index >= prevouts.size()) break;
                if (readers_full.load(std::memory_order_relaxed)) break;
                std::vector<uint8_t> stored_bytes;
                Coin coin;
                try {
                    if (first_read) {
                        first_read = false;
                        if (g_coin_prefetch_worker_gate) g_coin_prefetch_worker_gate();
                    }
                    const auto lmdb_start{SteadyClock::now()};
                    if (!db.ReadStoredCoinBytesForPrefetch(prevouts[index], stored_bytes)) continue;
                    if (bench) {
                        lmdb_us.fetch_add(static_cast<uint64_t>(Ticks<std::chrono::microseconds>(SteadyClock::now() - lmdb_start)),
                                          std::memory_order_relaxed);
                    }
                    const auto decode_start{SteadyClock::now()};
                    if (!DecodeCoinValue(stored_bytes, zstd, coin)) continue;
                    if (bench) {
                        decode_us.fetch_add(static_cast<uint64_t>(Ticks<std::chrono::microseconds>(SteadyClock::now() - decode_start)),
                                            std::memory_order_relaxed);
                    }
                    results[index] = std::move(coin);
                } catch (const dbwrapper_error& e) {
                    if (IsLMDBReadersFullError(e)) {
                        if (!readers_full_logged.exchange(true, std::memory_order_relaxed)) {
                            const int reclaimed{db.ReclaimStaleReaders()};
                            SetCoinPrefetchCooldown();
                            if (reclaimed < 0) {
                                LogPrintLevel(BCLog::LEVELDB, BCLog::Level::Warning,
                                              "Parallel coin prefetch hit MDB_READERS_FULL; falling back to serial for block "
                                              "(reader reclaim failed)\n");
                            } else if (reclaimed > 0) {
                                LogPrintLevel(BCLog::LEVELDB, BCLog::Level::Warning,
                                              "Parallel coin prefetch hit MDB_READERS_FULL; falling back to serial for block "
                                              "(reclaimed %d stale readers)\n",
                                              reclaimed);
                            } else {
                                LogPrintLevel(BCLog::LEVELDB, BCLog::Level::Warning,
                                              "Parallel coin prefetch hit MDB_READERS_FULL; falling back to serial for block "
                                              "(no stale readers reclaimed)\n");
                            }
                            util::BenchStatsInc(util::g_benchstats.coin_prefetch_readers_full);
                        }
                        readers_full.store(true, std::memory_order_relaxed);
                    } else {
                        throw;
                    }
                }
            }
            db.ReleaseThreadLocalReadTxn();
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    // Worker threads cache read txns in thread-local storage; reclaim slots before returning.
    db.ReclaimStaleReaders();
    if (bench) {
        util::g_benchstats.coin_prefetch_lmdb_us.fetch_add(lmdb_us.load(std::memory_order_relaxed),
                                                           std::memory_order_relaxed);
        util::g_benchstats.coin_prefetch_decode_us.fetch_add(decode_us.load(std::memory_order_relaxed),
                                                             std::memory_order_relaxed);
    }
    if (readers_full.load(std::memory_order_relaxed)) return false;
    for (size_t i = 0; i < prevouts.size(); ++i) {
        if (!results[i]) continue;
        out.push_back(PrefetchedCoin{prevouts[i], std::move(*results[i])});
    }
    return true;
}

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

int ComputeCoinPrefetchWorkers(const int requested_workers, const unsigned int max_readers)
{
    if (requested_workers < 2) return 0;
    const unsigned int available{max_readers > COIN_PREFETCH_READER_RESERVE
        ? max_readers - COIN_PREFETCH_READER_RESERVE
        : 0};
    const int slot_cap{static_cast<int>(available / COIN_PREFETCH_SLOTS_PER_WORKER)};
    return std::min({requested_workers, MAX_COIN_PREFETCH_PAR, slot_cap});
}

void ResetCoinPrefetchCooldownForTest()
{
    ClearCoinPrefetchCooldown();
}

void SetCoinPrefetchCooldownForTest()
{
    SetCoinPrefetchCooldown();
}

bool CoinPrefetchInCooldownForTest()
{
    return CoinPrefetchInCooldown();
}

void SetCoinPrefetchWorkerGateForTest(std::function<void()> gate)
{
    g_coin_prefetch_worker_gate = std::move(gate);
}

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
    if (m_db_params.memory_only) {
        // In-memory DBs cannot be resized without losing data.
        return;
    }
    // LMDB: keep the environment open. Reopening would close MDB_env while other threads
    // may still hold thread-local read transactions (see dbwrapper.cpp), which caused
    // shutdown SIGSEGVs after dumptxoutset/assumeutxo cache growth. maxreaders is sized
    // at env creation; map size auto-grows on MDB_MAP_FULL.
    m_db_params.cache_bytes = new_cache_size;
}

bool CCoinsViewDB::ReadCoinValue(const COutPoint& outpoint, Coin& coin) const
{
    CoinDBValue stored;
    if (!m_db->Read(CoinEntry(&outpoint), stored)) return false;
    return DecodeCoinValue(stored.bytes, m_utxo_zstd, coin);
}

bool CCoinsViewDB::ReadCoinValueForPrefetch(const COutPoint& outpoint, Coin& coin) const
{
    return ReadCoinValue(outpoint, coin);
}

bool CCoinsViewDB::ReadStoredCoinBytesForPrefetch(const COutPoint& outpoint, std::vector<uint8_t>& bytes) const
{
    CoinDBValue stored;
    if (!m_db->Read(CoinEntry(&outpoint), stored)) return false;
    bytes = std::move(stored.bytes);
    return true;
}

bool ParallelPrefetchCoins(CCoinsViewDB& db,
                           const std::vector<COutPoint>& prevouts,
                           std::vector<PrefetchedCoin>& out,
                           const int num_workers)
{
    if (num_workers < 2 || prevouts.empty() || CoinPrefetchInCooldown()) return false;
    // Validation (or tests) may hold a thread-local read txn; release it for worker budget.
    db.ReleaseThreadLocalReadTxn();
    return ParallelPrefetchCoinsImpl(db, prevouts, out, num_workers, db.GetUtxoZstdDictionary());
}

unsigned int CCoinsViewDB::GetMaxReaders() const
{
    return m_db->GetMaxReaders();
}

int CCoinsViewDB::ReclaimStaleReaders() const
{
    return m_db->ReclaimStaleReaders();
}

void CCoinsViewDB::ReleaseThreadLocalReadTxn() const
{
    m_db->ReleaseThreadLocalReadTxn();
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

bool CCoinsViewDB::WritePendingToDisk(const uint256& hashBlock, std::vector<PendingCoinWrite>& pending, size_t count, size_t changed)
{
    assert(!hashBlock.IsNull());

    const bool sync_writes = m_options.lmdbsync;
    const bool sync_final = sync_writes || m_sync_final_batch.load(std::memory_order_relaxed);

    struct SyncFinalBatchReset {
        std::atomic<bool>& flag;
        ~SyncFinalBatchReset() { flag.store(false, std::memory_order_relaxed); }
    } sync_final_reset{m_sync_final_batch};

    auto encode_start{std::chrono::steady_clock::now()};
    auto encode_elapsed{std::chrono::milliseconds::zero()};
    auto pause_encode_time{[&] {
        encode_elapsed += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - encode_start);
    }};
    auto resume_encode_time{[&] {
        encode_start = std::chrono::steady_clock::now();
    }};

    const bool parallel_encode{m_options.utxo_zstd
        && m_options.utxo_encode_workers >= 2
        && changed >= UTXO_ENCODE_PARALLEL_THRESHOLD};
    if (parallel_encode) {
        ParallelEncodeCoinValues(pending, m_options, m_utxo_zstd, m_options.utxo_encode_workers);
    }

    CDBBatch batch(*m_db);

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

    for (PendingCoinWrite& entry : pending) {
        CoinEntry coin_entry(&entry.outpoint);
        if (entry.spent) {
            batch.Erase(coin_entry);
        } else if (parallel_encode) {
            CoinDBValue stored;
            stored.bytes = std::move(entry.encoded);
            batch.Write(coin_entry, stored);
        } else {
            WriteCoinValue(batch, entry.outpoint, entry.coin);
        }
        if (batch.SizeEstimate() > m_options.batch_write_bytes) {
            pause_encode_time();
            LogDebug(BCLog::COINDB, "Writing partial batch of %.2f MiB\n", batch.SizeEstimate() * (1.0 / 1048576.0));
            {
                const auto lmdb_start{SteadyClock::now()};
                LOG_TIME_MILLIS_WITH_CATEGORY("write coins partial batch to LMDB", BCLog::BENCH);
                m_db->WriteBatch(batch, sync_writes);
                util::BenchStatsAdd(util::g_benchstats.flush_lmdb_us,
                                    static_cast<uint64_t>(Ticks<std::chrono::microseconds>(SteadyClock::now() - lmdb_start)));
            }
            batch.Clear();
            if (m_options.simulate_crash_ratio) {
                static FastRandomContext rng;
                if (rng.randrange(m_options.simulate_crash_ratio) == 0) {
                    LogError("Simulating a crash. Goodbye.");
                    _Exit(0);
                }
            }
            resume_encode_time();
        }
    }

    // In the last batch, mark the database as consistent with hashBlock again.
    batch.Erase(DB_HEAD_BLOCKS);
    batch.Write(DB_BEST_BLOCK, hashBlock);

    pause_encode_time();
    util::BenchStatsAdd(util::g_benchstats.flush_encode_us,
                        static_cast<uint64_t>(Ticks<std::chrono::microseconds>(encode_elapsed)));
    LogDebug(BCLog::BENCH, "BatchWrite: encode coins for db batch completed (%.2fms%s)\n",
             Ticks<MillisecondsDouble>(encode_elapsed),
             parallel_encode ? ", parallel" : "");
    LogDebug(BCLog::COINDB, "Writing final batch of %.2f MiB\n", batch.SizeEstimate() * (1.0 / 1048576.0));
    bool ret;
    {
        const auto lmdb_start{SteadyClock::now()};
        LOG_TIME_MILLIS_WITH_CATEGORY("write coins final batch to LMDB", BCLog::BENCH);
        ret = m_db->WriteBatch(batch, sync_final);
        util::BenchStatsAdd(util::g_benchstats.flush_lmdb_us,
                            static_cast<uint64_t>(Ticks<std::chrono::microseconds>(SteadyClock::now() - lmdb_start)));
    }
    LogDebug(BCLog::COINDB, "Committed %u changed transaction outputs (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    return ret;
}

bool CCoinsViewDB::BatchWriteFromSnapshot(const CoinsFlushSnapshot& snapshot)
{
    assert(!snapshot.hashBlock.IsNull());
    std::vector<PendingCoinWrite> pending;
    pending.reserve(snapshot.entries.size());
    for (const CoinsFlushSnapshotEntry& entry : snapshot.entries) {
        PendingCoinWrite pending_entry;
        pending_entry.outpoint = entry.outpoint;
        pending_entry.spent = entry.spent;
        if (!entry.spent) {
            pending_entry.coin = entry.coin;
        }
        pending.push_back(std::move(pending_entry));
    }
    return WritePendingToDisk(snapshot.hashBlock, pending, snapshot.entries.size(), snapshot.dirty_count);
}

bool CCoinsViewDB::BatchWrite(CoinsViewCacheCursor& cursor, const uint256 &hashBlock) {
    size_t count = 0;
    size_t changed = 0;
    assert(!hashBlock.IsNull());

    const bool defer_cache_finalize{cursor.DeferCacheFinalization()};

    std::vector<PendingCoinWrite> pending;
    pending.reserve(1024);
    for (auto it{cursor.Begin()}; it != cursor.End();) {
        if (it->second.IsDirty()) {
            PendingCoinWrite entry;
            entry.outpoint = it->first;
            entry.spent = it->second.coin.IsSpent();
            if (!entry.spent) {
                if (cursor.WillErase(*it)) {
                    entry.coin = std::move(it->second.coin);
                } else {
                    entry.coin = it->second.coin;
                }
            }
            if (defer_cache_finalize) {
                entry.cache_pair = it;
            }
            pending.push_back(std::move(entry));
            changed++;
        }
        count++;
        if (defer_cache_finalize) {
            it = cursor.Next(*it);
        } else {
            it = cursor.NextAndMaybeErase(*it);
        }
    }

    const bool ret{WritePendingToDisk(hashBlock, pending, count, changed)};
    if (ret && defer_cache_finalize) {
        for (PendingCoinWrite& entry : pending) {
            if (entry.cache_pair) {
                cursor.FinalizeEntry(*entry.cache_pair);
            }
        }
    }
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
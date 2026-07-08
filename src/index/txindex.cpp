// Copyright (c) 2017-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/txindex.h>

#include <clientversion.h>
#include <common/args.h>
#include <index/disktxpos.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <streams.h>
#include <util/benchstats.h>
#include <util/time.h>
#include <validation.h>

constexpr uint8_t DB_TXINDEX{'t'};

std::unique_ptr<TxIndex> g_txindex;


/** Access to the txindex database (indexes/txindex/) */
class TxIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    /// Read the disk location of the transaction data with the given hash. Returns false if the
    /// transaction hash is not indexed.
    bool ReadTxPos(const uint256& txid, CDiskTxPos& pos) const;

    /// Write a batch of transaction positions to the DB.
    [[nodiscard]] bool WriteTxs(const std::vector<std::pair<uint256, CDiskTxPos>>& v_pos);
};

TxIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "txindex", n_cache_size, f_memory, f_wipe, /*f_obfuscate=*/true)
{}

bool TxIndex::DB::ReadTxPos(const uint256 &txid, CDiskTxPos& pos) const
{
    return Read(std::make_pair(DB_TXINDEX, txid), pos);
}

bool TxIndex::DB::WriteTxs(const std::vector<std::pair<uint256, CDiskTxPos>>& v_pos)
{
    if (v_pos.empty()) return true;
    const auto write_start{SteadyClock::now()};
    CDBBatch batch(*this);
    for (const auto& tuple : v_pos) {
        batch.Write(std::make_pair(DB_TXINDEX, tuple.first), tuple.second);
    }
    const bool ok{WriteBatch(batch)};
    util::BenchStatsAdd(util::g_benchstats.txindex_write_us,
                        static_cast<uint64_t>(Ticks<std::chrono::microseconds>(SteadyClock::now() - write_start)));
    return ok;
}

TxIndex::TxIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe,
                 const unsigned int batch_blocks)
    : BaseIndex(std::move(chain), "txindex"),
      m_db(std::make_unique<TxIndex::DB>(n_cache_size, f_memory, f_wipe)),
      m_batch_blocks{batch_blocks > 0 ? batch_blocks : 1}
{}

TxIndex::~TxIndex()
{
    if (!FlushPendingWrites()) {
        LogPrintf("Warning: failed to flush pending txindex writes on shutdown\n");
    }
}

bool TxIndex::FlushPendingWrites()
{
    if (m_pending_writes.empty()) return true;
    if (!m_db->WriteTxs(m_pending_writes)) return false;
    m_pending_writes.clear();
    m_pending_block_count = 0;
    return true;
}

bool TxIndex::FlushPendingIndexWrites()
{
    return FlushPendingWrites();
}

bool TxIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    // Exclude genesis block transaction because outputs are not spendable.
    if (block.height == 0) return true;

    assert(block.data);
    CDiskTxPos pos({block.file_number, block.data_pos}, GetSizeOfCompactSize(block.data->vtx.size()));
    m_pending_writes.reserve(m_pending_writes.size() + block.data->vtx.size());
    for (const auto& tx : block.data->vtx) {
        m_pending_writes.emplace_back(tx->GetHash(), pos);
        pos.nTxOffset += ::GetSerializeSize(TX_WITH_WITNESS(*tx));
    }
    ++m_pending_block_count;
    const bool ibd{m_chainstate && m_chainstate->m_chainman.IsInitialBlockDownload()};
    if (!ibd || m_pending_block_count >= m_batch_blocks) {
        return FlushPendingWrites();
    }
    return true;
}

BaseIndex::DB& TxIndex::GetDB() const { return *m_db; }

bool TxIndex::FindTx(const uint256& tx_hash, uint256& block_hash, CTransactionRef& tx) const
{
    CDiskTxPos postx;
    if (!m_db->ReadTxPos(tx_hash, postx)) {
        return false;
    }

    std::vector<uint8_t> block_data;
    const FlatFilePos block_pos{postx.nFile, postx.nPos};
    if (!m_chainstate->m_blockman.ReadRawBlock(block_data, block_pos)) {
        LogError("%s: ReadRawBlock failed\n", __func__);
        return false;
    }
    CBlockHeader header;
    try {
        SpanReader reader{block_data};
        reader >> header;
        reader.ignore(postx.nTxOffset);
        reader >> TX_WITH_WITNESS(tx);
    } catch (const std::exception& e) {
        LogError("%s: Deserialize or I/O error - %s\n", __func__, e.what());
        return false;
    }
    if (tx->GetHash() != tx_hash) {
        LogError("%s: txid mismatch\n", __func__);
        return false;
    }
    block_hash = header.GetHash();
    return true;
}
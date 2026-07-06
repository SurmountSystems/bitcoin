// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockstorage.h>

#include <compress/dict_bootstrap.h>
#include <compress/dict_classify.h>
#include <compress/zstd.h>
#include <node/blockfile_format.h>
#include <arith_uint256.h>
#include <consensus/consensus.h>
#include <chain.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <flatfile.h>
#include <hash.h>
#include <kernel/blockmanager_opts.h>
#include <kernel/chainparams.h>
#include <kernel/messagestartchars.h>
#include <kernel/notifications_interface.h>
#include <logging.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <serialize.h>
#include <signet.h>
#include <span.h>
#include <streams.h>
#include <sync.h>
#include <tinyformat.h>
#include <uint256.h>
#include <undo.h>
#include <util/batchpriority.h>
#include <util/benchstats.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/ioprio.h>
#include <util/obfuscation.h>
#include <util/overflow.h>
#include <util/signalinterrupt.h>
#include <util/strencodings.h>
#include <util/syserror.h>
#include <util/threadnames.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <cstddef>
#include <mutex>
#include <thread>
#include <map>
#include <optional>
#include <ranges>
#include <unordered_map>

namespace {

bool DecompressBlockPayloadImpl(const compress::BlockZstd& zstd,
                                std::span<const uint8_t> compressed,
                                std::vector<uint8_t>& payload,
                                size_t max_output)
{
    return zstd.Decompress(compressed, payload, max_output);
}

const compress::BlockZstd& BlockDictForDecompress(const uint8_t flags,
                                                  const compress::BlockZstd& fallback)
{
    if ((flags & node::BLOCK_SERIALIZATION_FLAG_BUCKET_MASK) == 0) {
        return fallback;
    }
    const uint8_t bucket_id{node::BlockBucketFromFlags(flags)};
    if (compress::g_dict_bootstrap && compress::g_dict_bootstrap->UseTypedBlockFormat()
        && bucket_id < compress::NUM_BLOCK_BUCKETS) {
        const compress::DictZstd& typed{
            compress::g_dict_bootstrap->BlockDict(static_cast<compress::BlockBucket>(bucket_id))};
        if (typed) return typed;
        LogPrintf("Warning: missing typed block dictionary for bucket %s; falling back to monolithic dictionary\n",
                  compress::BlockBucketName(static_cast<compress::BlockBucket>(bucket_id)));
    }
    return fallback;
}

} // namespace

namespace kernel {
static constexpr uint8_t DB_BLOCK_FILES{'f'};
static constexpr uint8_t DB_BLOCK_INDEX{'b'};
static constexpr uint8_t DB_FLAG{'F'};
static constexpr uint8_t DB_REINDEX_FLAG{'R'};
static constexpr uint8_t DB_LAST_BLOCK{'l'};
static constexpr uint8_t DB_PRUNE_LOCK{'L'};
// Keys used in previous version that might still be found in the DB:
// BlockTreeDB::DB_TXINDEX_BLOCK{'T'};
// BlockTreeDB::DB_TXINDEX{'t'}
// BlockTreeDB::ReadFlag("txindex")

bool BlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo& info)
{
    return Read(std::make_pair(DB_BLOCK_FILES, nFile), info);
}

bool BlockTreeDB::WriteReindexing(bool fReindexing)
{
    if (fReindexing) {
        return Write(DB_REINDEX_FLAG, uint8_t{'1'});
    } else {
        return Erase(DB_REINDEX_FLAG);
    }
}

void BlockTreeDB::ReadReindexing(bool& fReindexing)
{
    fReindexing = Exists(DB_REINDEX_FLAG);
}

bool BlockTreeDB::ReadLastBlockFile(int& nFile)
{
    return Read(DB_LAST_BLOCK, nFile);
}

bool BlockTreeDB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*>>& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo, const std::unordered_map<std::string, node::PruneLockInfo>& prune_locks)
{
    CDBBatch batch(*this);
    for (const auto& [file, info] : fileInfo) {
        batch.Write(std::make_pair(DB_BLOCK_FILES, file), *info);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (const CBlockIndex* bi : blockinfo) {
        batch.Write(std::make_pair(DB_BLOCK_INDEX, bi->GetBlockHash()), CDiskBlockIndex{bi});
    }
    for (const auto& prune_lock : prune_locks) {
        if (prune_lock.second.temporary) continue;
        batch.Write(std::make_pair(DB_PRUNE_LOCK, prune_lock.first), prune_lock.second);
    }
    return WriteBatch(batch, true);
}

bool BlockTreeDB::WriteBatchSync(const node::BlockIndexWriteBatch& index_batch)
{
    CDBBatch batch(*this);
    for (const auto& [file, info] : index_batch.file_info) {
        batch.Write(std::make_pair(DB_BLOCK_FILES, file), info);
    }
    batch.Write(DB_LAST_BLOCK, index_batch.last_file);
    for (const auto& [hash, disk_index] : index_batch.block_indices) {
        batch.Write(std::make_pair(DB_BLOCK_INDEX, hash), disk_index);
    }
    for (const auto& prune_lock : index_batch.prune_locks) {
        if (prune_lock.second.temporary) continue;
        batch.Write(std::make_pair(DB_PRUNE_LOCK, prune_lock.first), prune_lock.second);
    }
    return WriteBatch(batch, true);
}

bool BlockTreeDB::WritePruneLock(const std::string& name, const node::PruneLockInfo& lock_info) {
    if (lock_info.temporary) return true;
    return Write(std::make_pair(DB_PRUNE_LOCK, name), lock_info);
}

bool BlockTreeDB::DeletePruneLock(const std::string& name) {
    return Erase(std::make_pair(DB_PRUNE_LOCK, name));
}

bool BlockTreeDB::LoadPruneLocks(std::unordered_map<std::string, node::PruneLockInfo>& prune_locks, const util::SignalInterrupt& interrupt) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    for (pcursor->Seek(DB_PRUNE_LOCK); pcursor->Valid(); pcursor->Next()) {
        if (interrupt) return false;

        std::pair<uint8_t, std::string> key;
        if ((!pcursor->GetKey(key)) || key.first != DB_PRUNE_LOCK) break;

        node::PruneLockInfo& lock_info = prune_locks[key.second];
        if (!pcursor->GetValue(lock_info)) {
            LogError("%s: failed to %s prune lock '%s'\n", __func__, "read", key.second);
            return false;
        }
        lock_info.temporary = false;
    }

    return true;
}

bool BlockTreeDB::WriteFlag(const std::string& name, bool fValue)
{
    return Write(std::make_pair(DB_FLAG, name), fValue ? uint8_t{'1'} : uint8_t{'0'});
}

bool BlockTreeDB::ReadFlag(const std::string& name, bool& fValue)
{
    uint8_t ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch)) {
        return false;
    }
    fValue = ch == uint8_t{'1'};
    return true;
}

bool BlockTreeDB::LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex, const util::SignalInterrupt& interrupt)
{
    AssertLockHeld(::cs_main);
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    pcursor->Seek(std::make_pair(DB_BLOCK_INDEX, uint256()));

    // Load m_block_index
    while (pcursor->Valid()) {
        if (interrupt) return false;
        std::pair<uint8_t, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = insertBlockIndex(diskindex.ConstructBlockHash());
                pindexNew->pprev          = insertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nTx            = diskindex.nTx;

                if (!CheckProofOfWork(pindexNew->GetBlockHash(), pindexNew->nBits, consensusParams)) {
                    LogError("%s: CheckProofOfWork failed: %s\n", __func__, pindexNew->ToString());
                    return false;
                }

                pcursor->Next();
            } else {
                LogError("%s: failed to read value\n", __func__);
                return false;
            }
        } else {
            break;
        }
    }

    return true;
}
} // namespace kernel

namespace node {

bool CBlockIndexWorkComparator::operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
{
    // First sort by most total work, ...
    if (pa->nChainWork != pb->nChainWork) {
        return pa->nChainWork < pb->nChainWork;
    }

    // ... then by earliest activatable time, ...
    if (pa->nSequenceId != pb->nSequenceId) {
        return pa->nSequenceId > pb->nSequenceId;
    }

    // Use pointer address as tie breaker (should only happen with blocks
    // loaded from disk, as those share the same id: 0 for blocks on the
    // best chain, 1 for all others).
    return pa > pb;
}

bool CBlockIndexHeightOnlyComparator::operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
{
    return pa->nHeight < pb->nHeight;
}

/** The number of blocks to keep below the deepest prune lock.
 *  There is nothing special about this number. It is higher than what we
 *  expect to see in regular mainnet reorgs, but not so high that it would
 *  noticeably interfere with the pruning mechanism.
 * */
static constexpr int PRUNE_LOCK_BUFFER{10};

std::vector<CBlockIndex*> BlockManager::GetAllBlockIndices()
{
    AssertLockHeld(cs_main);
    std::vector<CBlockIndex*> rv;
    rv.reserve(m_block_index.size());
    for (auto& [_, block_index] : m_block_index) {
        rv.push_back(&block_index);
    }
    return rv;
}

CBlockIndex* BlockManager::LookupBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);
    BlockMap::iterator it = m_block_index.find(hash);
    return it == m_block_index.end() ? nullptr : &it->second;
}

const CBlockIndex* BlockManager::LookupBlockIndex(const uint256& hash) const
{
    AssertLockHeld(cs_main);
    BlockMap::const_iterator it = m_block_index.find(hash);
    return it == m_block_index.end() ? nullptr : &it->second;
}

CBlockIndex* BlockManager::AddToBlockIndex(const CBlockHeader& block, CBlockIndex*& best_header)
{
    AssertLockHeld(cs_main);

    auto [mi, inserted] = m_block_index.try_emplace(block.GetHash(), block);
    if (!inserted) {
        return &mi->second;
    }
    CBlockIndex* pindexNew = &(*mi).second;

    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = SEQ_ID_INIT_FROM_DISK;

    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = m_block_index.find(block.hashPrevBlock);
    if (miPrev != m_block_index.end()) {
        pindexNew->pprev = &(*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    pindexNew->nTimeMax = (pindexNew->pprev ? std::max(pindexNew->pprev->nTimeMax, pindexNew->nTime) : pindexNew->nTime);
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    if (best_header == nullptr || best_header->nChainWork < pindexNew->nChainWork) {
        best_header = pindexNew;
    }

    m_dirty_blockindex.insert(pindexNew);

    return pindexNew;
}

void BlockManager::PruneOneBlockFile(const int fileNumber)
{
    AssertLockHeld(cs_main);
    LOCK(cs_LastBlockFile);

    for (auto& entry : m_block_index) {
        CBlockIndex* pindex = &entry.second;
        if (pindex->nFile == fileNumber) {
            pindex->nStatus &= ~BLOCK_HAVE_DATA;
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            m_dirty_blockindex.insert(pindex);

            // Prune from m_blocks_unlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // m_blocks_unlinked or setBlockIndexCandidates.
            auto range = m_blocks_unlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator _it = range.first;
                range.first++;
                if (_it->second == pindex) {
                    m_blocks_unlinked.erase(_it);
                }
            }
        }
    }

    m_blockfile_info.at(fileNumber) = CBlockFileInfo{};
    m_dirty_fileinfo.insert(fileNumber);
}

bool BlockManager::DoPruneLocksForbidPruning(const CBlockFileInfo& block_file_info)
{
    AssertLockHeld(cs_main);
    for (const auto& prune_lock : m_prune_locks) {
        if (prune_lock.second.height_first == std::numeric_limits<uint64_t>::max()) continue;
        // Remove the buffer and one additional block here to get actual height that is outside of the buffer
        const uint64_t lock_height{(prune_lock.second.height_first <= PRUNE_LOCK_BUFFER + 1) ? 1 : (prune_lock.second.height_first - PRUNE_LOCK_BUFFER - 1)};
        const uint64_t lock_height_last{SaturatingAdd(prune_lock.second.height_last, (uint64_t)PRUNE_LOCK_BUFFER)};
        if (block_file_info.nHeightFirst > lock_height_last) continue;
        if (block_file_info.nHeightLast <= lock_height) continue;
        // TODO: Check each block within the file against the prune_lock range

        LogDebug(BCLog::PRUNE, "%s limited pruning to height %d\n", prune_lock.first, lock_height);
        return true;
    }
    return false;
}

void BlockManager::FindFilesToPruneManual(
    std::set<int>& setFilesToPrune,
    int nManualPruneHeight,
    const Chainstate& chain,
    ChainstateManager& chainman)
{
    assert(IsPruneMode() && nManualPruneHeight > 0);

    LOCK2(cs_main, cs_LastBlockFile);
    if (chain.m_chain.Height() < 0) {
        return;
    }

    const auto [min_block_to_prune, last_block_can_prune] = chainman.GetPruneRange(chain, nManualPruneHeight);

    int count = 0;
    for (int fileNumber = 0; fileNumber < this->MaxBlockfileNum(); fileNumber++) {
        const auto& fileinfo = m_blockfile_info[fileNumber];
        if (fileinfo.nSize == 0 || fileinfo.nHeightLast > (unsigned)last_block_can_prune || fileinfo.nHeightFirst < (unsigned)min_block_to_prune) {
            continue;
        }

        if (DoPruneLocksForbidPruning(m_blockfile_info[fileNumber])) continue;

        PruneOneBlockFile(fileNumber);
        setFilesToPrune.insert(fileNumber);
        count++;
    }
    LogPrintf("[%s] Prune (Manual): prune_height=%d removed %d blk/rev pairs\n",
        chain.GetRole(), last_block_can_prune, count);
}

uint64_t BlockManager::GetPruneTargetForChainstate(const Chainstate& chain, ChainstateManager& chainman) const
{
    const auto number_of_chainstates{chainman.GetAll().size()};
    const uint64_t min_overall_target{MIN_DISK_SPACE_FOR_BLOCK_FILES * number_of_chainstates};
    auto target = std::max(min_overall_target, GetPruneTarget());
    uint64_t target_boost{0};
    if (m_opts.prune_target_during_init > -1 && chainman.IsInitialBlockDownload()) {
        if ((uint64_t)m_opts.prune_target_during_init <= target) {
            target = std::max(min_overall_target, (uint64_t)m_opts.prune_target_during_init);
        } else if (chain.GetRole() != ChainstateRole::ASSUMEDVALID) {
            // Only the background/normal gets the benefit
            // NOTE: This assumes only one such chainstate exists
            target_boost = m_opts.prune_target_during_init - target;
        }
    }
    // Distribute our -prune budget over all chainstates.
    target = (target / number_of_chainstates) + target_boost;
    return target;
}

void BlockManager::FindFilesToPrune(
    std::set<int>& setFilesToPrune,
    int last_prune,
    const Chainstate& chain,
    ChainstateManager& chainman)
{
    LOCK2(cs_main, cs_LastBlockFile);
    const auto target{GetPruneTargetForChainstate(chain, chainman)};
    const uint64_t target_sync_height = chainman.m_best_header->nHeight;

    if (chain.m_chain.Height() < 0 || target == 0) {
        return;
    }
    if (static_cast<uint64_t>(chain.m_chain.Height()) <= chainman.GetParams().PruneAfterHeight()) {
        return;
    }

    const auto [min_block_to_prune, last_block_can_prune] = chainman.GetPruneRange(chain, last_prune);

    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files
    // So we should leave a buffer under our target to account for another allocation
    // before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count = 0;

    if (nCurrentUsage + nBuffer >= target) {
        // On a prune event, the chainstate DB is flushed.
        // To avoid excessive prune events negating the benefit of high dbcache
        // values, we should not prune too rapidly.
        // So when pruning in IBD, increase the buffer to avoid a re-prune too soon.
        const auto chain_tip_height = chain.m_chain.Height();
        if (chainman.IsInitialBlockDownload() && target_sync_height > (uint64_t)chain_tip_height) {
            // Since this is only relevant during IBD, we assume blocks are at least 1 MB on average
            static constexpr uint64_t average_block_size = 1000000;  /* 1 MB */
            const uint64_t remaining_blocks = target_sync_height - chain_tip_height;
            nBuffer += average_block_size * remaining_blocks;
        }

        for (int fileNumber = 0; fileNumber < this->MaxBlockfileNum(); fileNumber++) {
            const auto& fileinfo = m_blockfile_info[fileNumber];
            nBytesToPrune = fileinfo.nSize + fileinfo.nUndoSize;

            if (fileinfo.nSize == 0) {
                continue;
            }

            if (nCurrentUsage + nBuffer < target) { // are we below our target?
                break;
            }

            // don't prune files that could have a block that's not within the allowable
            // prune range for the chain being pruned.
            if (fileinfo.nHeightLast > (unsigned)last_block_can_prune || fileinfo.nHeightFirst < (unsigned)min_block_to_prune) {
                continue;
            }

            if (DoPruneLocksForbidPruning(m_blockfile_info[fileNumber])) continue;

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogDebug(BCLog::PRUNE, "[%s] target=%dMiB actual=%dMiB diff=%dMiB min_height=%d max_prune_height=%d removed %d blk/rev pairs\n",
             chain.GetRole(), target / 1024 / 1024, nCurrentUsage / 1024 / 1024,
             (int64_t(target) - int64_t(nCurrentUsage)) / 1024 / 1024,
             min_block_to_prune, last_block_can_prune, count);
}

bool BlockManager::PruneLockExists(const std::string& name) const {
    return m_prune_locks.count(name);
}

bool BlockManager::UpdatePruneLock(const std::string& name, const PruneLockInfo& lock_info, const bool sync) {
    AssertLockHeld(::cs_main);
    if (sync) {
        if (!m_block_tree_db->WritePruneLock(name, lock_info)) {
            LogError("%s: failed to %s prune lock '%s'\n", __func__, "write", name);
            return false;
        }
    }
    PruneLockInfo& stored_lock_info = m_prune_locks[name];
    if (lock_info.temporary && !stored_lock_info.temporary) {
        // Erase non-temporary lock from disk
        if (!m_block_tree_db->DeletePruneLock(name)) {
            LogError("%s: failed to %s prune lock '%s'\n", __func__, "erase", name);
            return false;
        }
    }
    stored_lock_info = lock_info;
    return true;
}

bool BlockManager::DeletePruneLock(const std::string& name)
{
    AssertLockHeld(::cs_main);
    m_prune_locks.erase(name);

    // Since there is no reasonable expectation for any follow-up to this prune lock, actually ensure it gets committed to disk immediately
    if (!m_block_tree_db->DeletePruneLock(name)) {
        LogError("%s: failed to %s prune lock '%s'\n", __func__, "erase", name);
        return false;
    }
    return true;
}

CBlockIndex* BlockManager::InsertBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);

    if (hash.IsNull()) {
        return nullptr;
    }

    const auto [mi, inserted]{m_block_index.try_emplace(hash)};
    CBlockIndex* pindex = &(*mi).second;
    if (inserted) {
        pindex->phashBlock = &((*mi).first);
    }
    return pindex;
}

bool BlockManager::LoadBlockIndex(const std::optional<uint256>& snapshot_blockhash)
{
    if (!m_block_tree_db->LoadBlockIndexGuts(
            GetConsensus(), [this](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main) { return this->InsertBlockIndex(hash); }, m_interrupt)) {
        return false;
    }

    if (!m_block_tree_db->LoadPruneLocks(m_prune_locks, m_interrupt)) return false;

    if (snapshot_blockhash) {
        const std::optional<AssumeutxoData> maybe_au_data = GetParams().AssumeutxoForBlockhash(*snapshot_blockhash);
        if (!maybe_au_data) {
            m_opts.notifications.fatalError(strprintf(_("Assumeutxo data not found for the given blockhash '%s'."), snapshot_blockhash->ToString()));
            return false;
        }
        const AssumeutxoData& au_data = *Assert(maybe_au_data);
        m_snapshot_height = au_data.height;
        CBlockIndex* base{LookupBlockIndex(*snapshot_blockhash)};

        // Since m_chain_tx_count (responsible for estimated progress) isn't persisted
        // to disk, we must bootstrap the value for assumedvalid chainstates
        // from the hardcoded assumeutxo chainparams.
        base->m_chain_tx_count = au_data.m_chain_tx_count;
        LogPrintf("[snapshot] set m_chain_tx_count=%d for %s\n", au_data.m_chain_tx_count, snapshot_blockhash->ToString());
    } else {
        // If this isn't called with a snapshot blockhash, make sure the cached snapshot height
        // is null. This is relevant during snapshot completion, when the blockman may be loaded
        // with a height that then needs to be cleared after the snapshot is fully validated.
        m_snapshot_height.reset();
    }

    Assert(m_snapshot_height.has_value() == snapshot_blockhash.has_value());

    // Calculate nChainWork
    std::vector<CBlockIndex*> vSortedByHeight{GetAllBlockIndices()};
    std::sort(vSortedByHeight.begin(), vSortedByHeight.end(),
              CBlockIndexHeightOnlyComparator());

    CBlockIndex* previous_index{nullptr};
    for (CBlockIndex* pindex : vSortedByHeight) {
        if (m_interrupt) return false;
        if (previous_index && pindex->nHeight > previous_index->nHeight + 1) {
            LogError("%s: block index is non-contiguous, index of height %d missing\n", __func__, previous_index->nHeight + 1);
            return false;
        }
        previous_index = pindex;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        pindex->nTimeMax = (pindex->pprev ? std::max(pindex->pprev->nTimeMax, pindex->nTime) : pindex->nTime);

        // We can link the chain of blocks for which we've received transactions at some point, or
        // blocks that are assumed-valid on the basis of snapshot load (see
        // PopulateAndValidateSnapshot()).
        // Pruned nodes may have deleted the block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (m_snapshot_height && pindex->nHeight == *m_snapshot_height &&
                        pindex->GetBlockHash() == *snapshot_blockhash) {
                    // Should have been set above; don't disturb it with code below.
                    Assert(pindex->m_chain_tx_count > 0);
                } else if (pindex->pprev->m_chain_tx_count > 0) {
                    pindex->m_chain_tx_count = pindex->pprev->m_chain_tx_count + pindex->nTx;
                } else {
                    pindex->m_chain_tx_count = 0;
                    m_blocks_unlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->m_chain_tx_count = pindex->nTx;
            }
        }
        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev && (pindex->pprev->nStatus & BLOCK_FAILED_MASK)) {
            pindex->nStatus |= BLOCK_FAILED_CHILD;
            m_dirty_blockindex.insert(pindex);
        }
        if (pindex->pprev) {
            pindex->BuildSkip();
        }
    }

    return true;
}

BlockIndexWriteBatch BlockManager::PrepareBlockIndexWriteBatch()
{
    AssertLockHeld(::cs_main);
    BlockIndexWriteBatch batch;
    batch.file_info.reserve(m_dirty_fileinfo.size());
    for (const int file : m_dirty_fileinfo) {
        batch.file_info.emplace_back(file, m_blockfile_info[file]);
    }
    batch.block_indices.reserve(m_dirty_blockindex.size());
    for (const CBlockIndex* pindex : m_dirty_blockindex) {
        batch.block_indices.emplace_back(pindex->GetBlockHash(), CDiskBlockIndex{pindex});
    }
    batch.last_file = WITH_LOCK(cs_LastBlockFile, return this->MaxBlockfileNum());
    batch.prune_locks = m_prune_locks;
    return batch;
}

namespace {
bool DiskIndexMatches(const CBlockIndex& index, const CDiskBlockIndex& disk)
{
    return index.nHeight == disk.nHeight
        && index.nStatus == disk.nStatus
        && index.nTx == disk.nTx
        && index.nFile == disk.nFile
        && index.nDataPos == disk.nDataPos
        && index.nUndoPos == disk.nUndoPos
        && (index.pprev ? index.pprev->GetBlockHash() : uint256()) == disk.hashPrev;
}

bool BlockFileInfoMatches(const CBlockFileInfo& a, const CBlockFileInfo& b)
{
    return a.nBlocks == b.nBlocks
        && a.nSize == b.nSize
        && a.nUndoSize == b.nUndoSize
        && a.nHeightFirst == b.nHeightFirst
        && a.nHeightLast == b.nHeightLast
        && a.nTimeFirst == b.nTimeFirst
        && a.nTimeLast == b.nTimeLast;
}
} // namespace

void BlockManager::CommitBlockIndexWriteBatch(const BlockIndexWriteBatch& batch)
{
    AssertLockHeld(::cs_main);
    for (const auto& [file, disk_info] : batch.file_info) {
        if (m_dirty_fileinfo.contains(file) && BlockFileInfoMatches(m_blockfile_info[file], disk_info)) {
            m_dirty_fileinfo.erase(file);
        }
    }
    for (const auto& [hash, disk_index] : batch.block_indices) {
        if (CBlockIndex* pindex{LookupBlockIndex(hash)}) {
            if (m_dirty_blockindex.contains(pindex) && DiskIndexMatches(*pindex, disk_index)) {
                m_dirty_blockindex.erase(pindex);
            }
        }
    }
}

bool BlockManager::WriteBlockIndexBatch(const BlockIndexWriteBatch& batch)
{
    AssertLockHeld(m_cs_block_index_write);
    if (batch.empty()) {
        return true;
    }
    return m_block_tree_db->WriteBatchSync(batch);
}

bool BlockManager::WriteBlockIndexDB()
{
    BlockIndexWriteBatch batch;
    {
        LOCK(::cs_main);
        batch = PrepareBlockIndexWriteBatch();
    }
    {
        LOCK(m_cs_block_index_write);
        if (!WriteBlockIndexBatch(batch)) {
            return false;
        }
    }
    LOCK(::cs_main);
    CommitBlockIndexWriteBatch(batch);
    return true;
}

BlockReadLoc BlockManager::CopyBlockReadLocAssumingLockHeld(const CBlockIndex& index) const
{
    AssertLockHeld(::cs_main);
    BlockReadLoc loc;
    loc.hash = index.GetBlockHash();
    if (index.nStatus & BLOCK_HAVE_DATA) {
        loc.have_data = true;
        loc.pos.nFile = index.nFile;
        loc.pos.nPos = index.nDataPos;
    }
    return loc;
}

BlockReadLoc BlockManager::CopyBlockReadLoc(const CBlockIndex& index) const
{
    LOCK(::cs_main);
    return CopyBlockReadLocAssumingLockHeld(index);
}

UndoReadLoc BlockManager::CopyUndoReadLocAssumingLockHeld(const CBlockIndex& index) const
{
    AssertLockHeld(::cs_main);
    UndoReadLoc loc;
    if ((index.nStatus & BLOCK_HAVE_UNDO) && index.pprev) {
        loc.have_undo = true;
        loc.pos.nFile = index.nFile;
        loc.pos.nPos = index.nUndoPos;
        loc.prev_block_hash = index.pprev->GetBlockHash();
    }
    return loc;
}

UndoReadLoc BlockManager::CopyUndoReadLoc(const CBlockIndex& index) const
{
    LOCK(::cs_main);
    return CopyUndoReadLocAssumingLockHeld(index);
}

bool BlockManager::LoadBlockIndexDB(const std::optional<uint256>& snapshot_blockhash)
{
    if (!LoadBlockIndex(snapshot_blockhash)) {
        return false;
    }
    int max_blockfile_num{0};

    // Load block file info
    m_block_tree_db->ReadLastBlockFile(max_blockfile_num);
    m_blockfile_info.resize(max_blockfile_num + 1);
    LogPrintf("%s: last block file = %i\n", __func__, max_blockfile_num);
    for (int nFile = 0; nFile <= max_blockfile_num; nFile++) {
        m_block_tree_db->ReadBlockFileInfo(nFile, m_blockfile_info[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, m_blockfile_info[max_blockfile_num].ToString());
    for (int nFile = max_blockfile_num + 1; true; nFile++) {
        CBlockFileInfo info;
        if (m_block_tree_db->ReadBlockFileInfo(nFile, info)) {
            m_blockfile_info.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    for (const auto& [_, block_index] : m_block_index) {
        if (block_index.nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(block_index.nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++) {
        FlatFilePos pos(*it, 0);
        if (OpenBlockFile(pos, true).IsNull()) {
            return false;
        }
    }

    {
        // Initialize the blockfile cursors.
        LOCK(cs_LastBlockFile);
        for (size_t i = 0; i < m_blockfile_info.size(); ++i) {
            const auto last_height_in_file = m_blockfile_info[i].nHeightLast;
            m_blockfile_cursors[BlockfileTypeForHeight(last_height_in_file)] = {static_cast<int>(i), 0};
        }
    }

    // Check whether we have ever pruned block & undo files
    m_block_tree_db->ReadFlag("prunedblockfiles", m_have_pruned);
    if (m_have_pruned) {
        LogPrintf("LoadBlockIndexDB(): Block files have previously been pruned\n");
    }

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    m_block_tree_db->ReadReindexing(fReindexing);
    if (fReindexing) m_blockfiles_indexed = false;

    return true;
}

void BlockManager::ScanAndUnlinkAlreadyPrunedFiles()
{
    AssertLockHeld(::cs_main);
    int max_blockfile = WITH_LOCK(cs_LastBlockFile, return this->MaxBlockfileNum());
    if (!m_have_pruned) {
        return;
    }

    std::set<int> block_files_to_prune;
    for (int file_number = 0; file_number < max_blockfile; file_number++) {
        if (m_blockfile_info[file_number].nSize == 0) {
            block_files_to_prune.insert(file_number);
        }
    }

    UnlinkPrunedFiles(block_files_to_prune);
}

const CBlockIndex* BlockManager::GetLastCheckpoint(const CCheckpointData& data)
{
    const MapCheckpoints& checkpoints = data.mapCheckpoints;

    for (const MapCheckpoints::value_type& i : checkpoints | std::views::reverse) {
        const uint256& hash = i.second;
        const CBlockIndex* pindex = LookupBlockIndex(hash);
        if (pindex) {
            return pindex;
        }
    }
    return nullptr;
}

bool BlockManager::IsBlockPruned(const CBlockIndex& block) const
{
    AssertLockHeld(::cs_main);
    return m_have_pruned && !(block.nStatus & BLOCK_HAVE_DATA) && (block.nTx > 0);
}

const CBlockIndex* BlockManager::GetFirstBlock(const CBlockIndex& upper_block, uint32_t status_mask, const CBlockIndex* lower_block) const
{
    AssertLockHeld(::cs_main);
    const CBlockIndex* last_block = &upper_block;
    assert((last_block->nStatus & status_mask) == status_mask); // 'upper_block' must satisfy the status mask
    while (last_block->pprev && ((last_block->pprev->nStatus & status_mask) == status_mask)) {
        if (lower_block) {
            // Return if we reached the lower_block
            if (last_block == lower_block) return lower_block;
            // if range was surpassed, means that 'lower_block' is not part of the 'upper_block' chain
            // and so far this is not allowed.
            assert(last_block->nHeight >= lower_block->nHeight);
        }
        last_block = last_block->pprev;
    }
    assert(last_block != nullptr);
    return last_block;
}

bool BlockManager::CheckBlockDataAvailability(const CBlockIndex& upper_block, const CBlockIndex& lower_block)
{
    if (!(upper_block.nStatus & BLOCK_HAVE_DATA)) return false;
    return GetFirstBlock(upper_block, BLOCK_HAVE_DATA, &lower_block) == &lower_block;
}

// If we're using -prune with -reindex, then delete block files that will be ignored by the
// reindex.  Since reindexing works by starting at block file 0 and looping until a blockfile
// is missing, do the same here to delete any later block files after a gap.  Also delete all
// rev files since they'll be rewritten by the reindex anyway.  This ensures that m_blockfile_info
// is in sync with what's actually on disk by the time we start downloading, so that pruning
// works correctly.
void BlockManager::CleanupBlockRevFiles() const
{
    std::map<std::string, fs::path> mapBlockFiles;

    // Glob all blk?????.dat and rev?????.dat files from the blocks directory.
    // Remove the rev files immediately and insert the blk file paths into an
    // ordered map keyed by block file index.
    LogPrintf("Removing unusable blk?????.dat and rev?????.dat files for -reindex with -prune\n");
    for (fs::directory_iterator it(m_opts.blocks_dir); it != fs::directory_iterator(); it++) {
        const std::string path = fs::PathToString(it->path().filename());
        if (fs::is_regular_file(*it) &&
            path.length() == 12 &&
            path.substr(8,4) == ".dat")
        {
            if (path.substr(0, 3) == "blk") {
                mapBlockFiles[path.substr(3, 5)] = it->path();
            } else if (path.substr(0, 3) == "rev") {
                remove(it->path());
            }
        }
    }

    // Remove all block files that aren't part of a contiguous set starting at
    // zero by walking the ordered map (keys are block file indices) by
    // keeping a separate counter.  Once we hit a gap (or if 0 doesn't exist)
    // start removing block files.
    int nContigCounter = 0;
    for (const std::pair<const std::string, fs::path>& item : mapBlockFiles) {
        if (LocaleIndependentAtoi<int>(item.first) == nContigCounter) {
            nContigCounter++;
            continue;
        }
        remove(item.second);
    }
}

CBlockFileInfo* BlockManager::GetBlockFileInfo(size_t n)
{
    LOCK(cs_LastBlockFile);

    if (n >= m_blockfile_info.size()) return nullptr;
    return &m_blockfile_info.at(n);
}

bool BlockManager::ReadBlockUndo(CBlockUndo& blockundo, const CBlockIndex& index) const
{
    return ReadBlockUndo(blockundo, CopyUndoReadLoc(index));
}

bool BlockManager::ReadBlockUndo(CBlockUndo& blockundo, const UndoReadLoc& loc) const
{
    if (!loc.IsValid()) {
        return false;
    }

    // Open history file to read
    AutoFile file{OpenUndoFile(loc.pos, true)};
    if (file.IsNull()) {
        LogError("OpenUndoFile failed for %s while reading block undo", loc.pos.ToString());
        return false;
    }
    BufferedReader filein{std::move(file)};

    try {
    // Read block
    uint256 hashChecksum;
    HashVerifier verifier{filein}; // Use HashVerifier as reserializing may lose data, c.f. commit d342424301013ec47dc146a4beb49d5c9319d80a
        verifier << loc.prev_block_hash;
        verifier >> blockundo;
        filein >> hashChecksum;

    // Verify checksum
    if (hashChecksum != verifier.GetHash()) {
        LogError("%s: Checksum mismatch at %s\n", __func__, loc.pos.ToString());
        return false;
    }
    } catch (const std::exception& e) {
        LogError("Deserialize or I/O error - %s at %s while reading block undo", e.what(), loc.pos.ToString());
        return false;
    }

    return true;
}

bool BlockManager::FlushUndoFile(int block_file, bool finalize)
{
    FlatFilePos undo_pos_old(block_file, m_blockfile_info[block_file].nUndoSize);
    if (!m_undo_file_seq.Flush(undo_pos_old, finalize)) {
        m_opts.notifications.flushError(_("Flushing undo file to disk failed. This is likely the result of an I/O error."));
        return false;
    }
    return true;
}

bool BlockManager::FlushBlockFile(int blockfile_num, bool fFinalize, bool finalize_undo)
{
    bool success = true;
    LOCK(cs_LastBlockFile);

    if (m_blockfile_info.size() < 1) {
        // Return if we haven't loaded any blockfiles yet. This happens during
        // chainstate init, when we call ChainstateManager::MaybeRebalanceCaches() (which
        // then calls FlushStateToDisk()), resulting in a call to this function before we
        // have populated `m_blockfile_info` via LoadBlockIndexDB().
        return true;
    }
    assert(static_cast<int>(m_blockfile_info.size()) > blockfile_num);

    FlatFilePos block_pos_old(blockfile_num, m_blockfile_info[blockfile_num].nSize);
    if (!m_block_file_seq.Flush(block_pos_old, fFinalize)) {
        m_opts.notifications.flushError(_("Flushing block file to disk failed. This is likely the result of an I/O error."));
        success = false;
    }
    // we do not always flush the undo file, as the chain tip may be lagging behind the incoming blocks,
    // e.g. during IBD or a sync after a node going offline
    if (!fFinalize || finalize_undo) {
        if (!FlushUndoFile(blockfile_num, finalize_undo)) {
            success = false;
        }
    }
    return success;
}

BlockfileType BlockManager::BlockfileTypeForHeight(int height)
{
    if (!m_snapshot_height) {
        return BlockfileType::NORMAL;
    }
    return (height >= *m_snapshot_height) ? BlockfileType::ASSUMED : BlockfileType::NORMAL;
}

bool BlockManager::FlushChainstateBlockFile(int tip_height)
{
    LOCK(cs_LastBlockFile);
    auto& cursor = m_blockfile_cursors[BlockfileTypeForHeight(tip_height)];
    // If the cursor does not exist, it means an assumeutxo snapshot is loaded,
    // but no blocks past the snapshot height have been written yet, so there
    // is no data associated with the chainstate, and it is safe not to flush.
    if (cursor) {
        return FlushBlockFile(cursor->file_num, /*fFinalize=*/false, /*finalize_undo=*/false);
    }
    // No need to log warnings in this case.
    return true;
}

uint64_t BlockManager::CalculateCurrentUsage()
{
    LOCK(cs_LastBlockFile);

    uint64_t retval = 0;
    for (const CBlockFileInfo& file : m_blockfile_info) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

void BlockManager::UnlinkPrunedFiles(const std::set<int>& setFilesToPrune) const
{
    std::error_code ec;
    for (std::set<int>::iterator it = setFilesToPrune.begin(); it != setFilesToPrune.end(); ++it) {
        FlatFilePos pos(*it, 0);
        const bool removed_blockfile{fs::remove(m_block_file_seq.FileName(pos), ec)};
        const bool removed_undofile{fs::remove(m_undo_file_seq.FileName(pos), ec)};
        if (removed_blockfile || removed_undofile) {
            LogDebug(BCLog::BLOCKSTORAGE, "Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
        }
    }
}

AutoFile BlockManager::OpenBlockFile(const FlatFilePos& pos, bool fReadOnly) const
{
    return AutoFile{m_block_file_seq.Open(pos, fReadOnly), m_xor_key};
}

/** Open an undo file (rev?????.dat) */
AutoFile BlockManager::OpenUndoFile(const FlatFilePos& pos, bool fReadOnly) const
{
    return AutoFile{m_undo_file_seq.Open(pos, fReadOnly), m_xor_key};
}

fs::path BlockManager::GetBlockPosFilename(const FlatFilePos& pos) const
{
    return m_block_file_seq.FileName(pos);
}

FlatFilePos BlockManager::FindNextBlockPos(unsigned int nAddSize, unsigned int nHeight, uint64_t nTime)
{
    LOCK(cs_LastBlockFile);

    const BlockfileType chain_type = BlockfileTypeForHeight(nHeight);

    if (!m_blockfile_cursors[chain_type]) {
        // If a snapshot is loaded during runtime, we may not have initialized this cursor yet.
        assert(chain_type == BlockfileType::ASSUMED);
        const auto new_cursor = BlockfileCursor{this->MaxBlockfileNum() + 1};
        m_blockfile_cursors[chain_type] = new_cursor;
        LogDebug(BCLog::BLOCKSTORAGE, "[%s] initializing blockfile cursor to %s\n", chain_type, new_cursor);
    }
    const int last_blockfile = m_blockfile_cursors[chain_type]->file_num;

    int nFile = last_blockfile;
    if (static_cast<int>(m_blockfile_info.size()) <= nFile) {
        m_blockfile_info.resize(nFile + 1);
    }

    bool finalize_undo = false;
    unsigned int max_blockfile_size{MAX_BLOCKFILE_SIZE};
    // Use smaller blockfiles in test-only -fastprune mode - but avoid
    // the possibility of having a block not fit into the block file.
    if (m_opts.fast_prune) {
        max_blockfile_size = 0x10000; // 64kiB
        if (nAddSize >= max_blockfile_size) {
            // dynamically adjust the blockfile size to be larger than the added size
            max_blockfile_size = nAddSize + 1;
        }
    }
    assert(nAddSize < max_blockfile_size);

    while (m_blockfile_info[nFile].nSize + nAddSize >= max_blockfile_size) {
        // when the undo file is keeping up with the block file, we want to flush it explicitly
        // when it is lagging behind (more blocks arrive than are being connected), we let the
        // undo block write case handle it
        finalize_undo = (static_cast<int>(m_blockfile_info[nFile].nHeightLast) ==
                         Assert(m_blockfile_cursors[chain_type])->undo_height);

        // Try the next unclaimed blockfile number
        nFile = this->MaxBlockfileNum() + 1;
        // Set to increment MaxBlockfileNum() for next iteration
        m_blockfile_cursors[chain_type] = BlockfileCursor{nFile};

        if (static_cast<int>(m_blockfile_info.size()) <= nFile) {
            m_blockfile_info.resize(nFile + 1);
        }
    }
    FlatFilePos pos;
    pos.nFile = nFile;
    pos.nPos = m_blockfile_info[nFile].nSize;

    if (nFile != last_blockfile) {
        LogDebug(BCLog::BLOCKSTORAGE, "Leaving block file %i: %s (onto %i) (height %i)\n",
                 last_blockfile, m_blockfile_info[last_blockfile].ToString(), nFile, nHeight);

        // Do not propagate the return code. The flush concerns a previous block
        // and undo file that has already been written to. If a flush fails
        // here, and we crash, there is no expected additional block data
        // inconsistency arising from the flush failure here. However, the undo
        // data may be inconsistent after a crash if the flush is called during
        // a reindex. A flush error might also leave some of the data files
        // untrimmed.
        if (!FlushBlockFile(last_blockfile, /*fFinalize=*/true, finalize_undo)) {
            LogPrintLevel(BCLog::BLOCKSTORAGE, BCLog::Level::Warning,
                          "Failed to flush previous block file %05i (finalize=1, finalize_undo=%i) before opening new block file %05i\n",
                          last_blockfile, finalize_undo, nFile);
        }
        // No undo data yet in the new file, so reset our undo-height tracking.
        m_blockfile_cursors[chain_type] = BlockfileCursor{nFile};
    }

    m_blockfile_info[nFile].AddBlock(nHeight, nTime);
    m_blockfile_info[nFile].nSize += nAddSize;

    bool out_of_space;
    size_t bytes_allocated = m_block_file_seq.Allocate(pos, nAddSize, out_of_space);
    if (out_of_space) {
        m_opts.notifications.fatalError(_("Disk space is too low!"));
        return {};
    }
    if (bytes_allocated != 0 && IsPruneMode()) {
        m_check_for_pruning = true;
    }

    m_dirty_fileinfo.insert(nFile);
    return pos;
}

static compress::BlockZstd InitBlockZstd(const BlockManager::Options& opts)
{
    if (!opts.block_zstd && !opts.block_zstd_decompress) {
        return {};
    }

    fs::path dict_path{opts.block_zstd_dict};
    if (dict_path.empty()) {
        dict_path = compress::DefaultBlockDictionaryPath();
    }

    if (dict_path.empty()) {
        LogWarning("Block zstd dictionary path is not configured; compression and decompression are disabled\n");
        return {};
    }

    const auto dictionary{compress::LoadDictionaryFile(dict_path)};
    if (!dictionary) {
        LogWarning("Failed to load block zstd dictionary from %s; compression and decompression are disabled\n",
                   fs::PathToString(dict_path));
        return {};
    }

    compress::BlockZstd zstd{*dictionary};
    if (!zstd) {
        LogWarning("Failed to initialize block zstd dictionary from %s; compression and decompression are disabled\n",
                   fs::PathToString(dict_path));
        return {};
    }

    LogInfo("Loaded block zstd dictionary (%s bytes) from %s\n", dictionary->size(), fs::PathToString(dict_path));
    return zstd;
}

void BlockManager::UpdateBlockInfo(const CBlock& block, unsigned int nHeight, const FlatFilePos& pos,
                                   const std::optional<unsigned int> on_disk_payload_size)
{
    LOCK(cs_LastBlockFile);

    // Update the cursor so it points to the last file.
    const BlockfileType chain_type{BlockfileTypeForHeight(nHeight)};
    auto& cursor{m_blockfile_cursors[chain_type]};
    if (!cursor || cursor->file_num < pos.nFile) {
        m_blockfile_cursors[chain_type] = BlockfileCursor{pos.nFile};
    }

    // Update the file information with the current block.
    const unsigned int added_size{on_disk_payload_size.value_or(
        static_cast<unsigned int>(::GetSerializeSize(TX_WITH_WITNESS(block))))};
    const int nFile = pos.nFile;
    if (static_cast<int>(m_blockfile_info.size()) <= nFile) {
        m_blockfile_info.resize(nFile + 1);
    }
    m_blockfile_info[nFile].AddBlock(nHeight, block.GetBlockTime());
    m_blockfile_info[nFile].nSize = std::max(pos.nPos + added_size, m_blockfile_info[nFile].nSize);
    m_dirty_fileinfo.insert(nFile);
}

bool BlockManager::FindUndoPos(BlockValidationState& state, int nFile, FlatFilePos& pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    pos.nPos = m_blockfile_info[nFile].nUndoSize;
    m_blockfile_info[nFile].nUndoSize += nAddSize;
    m_dirty_fileinfo.insert(nFile);

    bool out_of_space;
    size_t bytes_allocated = m_undo_file_seq.Allocate(pos, nAddSize, out_of_space);
    if (out_of_space) {
        return FatalError(m_opts.notifications, state, _("Disk space is too low!"));
    }
    if (bytes_allocated != 0 && IsPruneMode()) {
        m_check_for_pruning = true;
    }

    return true;
}

bool BlockManager::WriteBlockUndo(const CBlockUndo& blockundo, BlockValidationState& state, CBlockIndex& block)
{
    AssertLockHeld(::cs_main);
    const BlockfileType type = BlockfileTypeForHeight(block.nHeight);
    auto& cursor = *Assert(WITH_LOCK(cs_LastBlockFile, return m_blockfile_cursors[type]));

    // Write undo information to disk
    if (block.GetUndoPos().IsNull()) {
        FlatFilePos pos;
        const auto blockundo_size{static_cast<uint32_t>(GetSerializeSize(blockundo))};
        if (!FindUndoPos(state, block.nFile, pos, blockundo_size + UNDO_DATA_DISK_OVERHEAD)) {
            LogError("FindUndoPos failed for %s while writing block undo", pos.ToString());
            return false;
        }

        // Open history file to append
            AutoFile file{OpenUndoFile(pos)};
            if (file.IsNull()) {
                LogError("OpenUndoFile failed for %s while writing block undo", pos.ToString());
            return FatalError(m_opts.notifications, state, _("Failed to write undo data."));
        }
        {
            BufferedWriter fileout{file};

        // Write index header
        fileout << GetParams().MessageStart() << blockundo_size;
        pos.nPos += BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE;
            {
                // Calculate checksum
                HashWriter hasher{};
                hasher << block.pprev->GetBlockHash() << blockundo;
                // Write undo data & checksum
                fileout << blockundo << hasher.GetHash();
            }
            // BufferedWriter will flush pending data to file when fileout goes out of scope.
        }

        // Make sure that the file is closed before we call `FlushUndoFile`.
        if (file.fclose() != 0) {
            LogError("Failed to close block undo file %s: %s", pos.ToString(), SysErrorString(errno));
            return FatalError(m_opts.notifications, state, _("Failed to close block undo file."));
        }

        // rev files are written in block height order, whereas blk files are written as blocks come in (often out of order)
        // we want to flush the rev (undo) file once we've written the last block, which is indicated by the last height
        // in the block file info as below; note that this does not catch the case where the undo writes are keeping up
        // with the block writes (usually when a synced up node is getting newly mined blocks) -- this case is caught in
        // the FindNextBlockPos function
        if (pos.nFile < cursor.file_num && static_cast<uint32_t>(block.nHeight) == m_blockfile_info[pos.nFile].nHeightLast) {
            // Do not propagate the return code, a failed flush here should not
            // be an indication for a failed write. If it were propagated here,
            // the caller would assume the undo data not to be written, when in
            // fact it is. Note though, that a failed flush might leave the data
            // file untrimmed.
            if (!FlushUndoFile(pos.nFile, true)) {
                LogPrintLevel(BCLog::BLOCKSTORAGE, BCLog::Level::Warning, "Failed to flush undo file %05i\n", pos.nFile);
            }
        } else if (pos.nFile == cursor.file_num && block.nHeight > cursor.undo_height) {
            cursor.undo_height = block.nHeight;
        }
        // update nUndoPos in block index
        block.nUndoPos = pos.nPos;
        block.nStatus |= BLOCK_HAVE_UNDO;
        m_dirty_blockindex.insert(&block);
    }

    return true;
}

void BlockDecompressPool::Start(int num_workers, const compress::BlockZstd* zstd)
{
    Stop();
    if (num_workers < 2 || !zstd) {
        m_num_workers = 0;
        return;
    }
    m_zstd = zstd;
    m_num_workers = num_workers;
    m_stop.store(false, std::memory_order_relaxed);
    m_workers.reserve(static_cast<size_t>(num_workers));
    for (int i = 0; i < num_workers; ++i) {
        m_workers.emplace_back([this] { WorkerLoop(); });
    }
}

void BlockDecompressPool::Stop()
{
    m_stop.store(true, std::memory_order_relaxed);
    {
        std::lock_guard lock{m_mutex};
        m_cv.notify_all();
    }
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) worker.join();
    }
    m_workers.clear();
    m_num_workers = 0;
    m_zstd = nullptr;
    std::lock_guard lock{m_mutex};
    m_queue.clear();
}

void BlockDecompressPool::WorkerLoop()
{
    util::ThreadRename("block-decompress");
    while (!m_stop.load(std::memory_order_relaxed)) {
        std::unique_ptr<Job> job;
        {
            std::unique_lock lock{m_mutex};
            m_cv.wait(lock, [&] { return m_stop.load(std::memory_order_relaxed) || !m_queue.empty(); });
            if (m_stop.load(std::memory_order_relaxed) && m_queue.empty()) return;
            if (m_queue.empty()) continue;
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        const bool ok{DecompressBlockPayloadImpl(*m_zstd, job->compressed, *job->payload, job->max_output)};
        job->done.set_value(ok);
    }
}

bool BlockDecompressPool::Submit(std::span<const uint8_t> compressed,
                                 std::vector<uint8_t>& payload,
                                 const size_t max_output) const
{
    if (!Active() || m_stop.load(std::memory_order_relaxed)) return false;
    auto job{std::make_unique<Job>()};
    job->compressed = compressed;
    job->payload = &payload;
    job->max_output = max_output;
    std::future<bool> fut{job->done.get_future()};
    {
        std::lock_guard lock{m_mutex};
        if (m_queue.size() >= MAX_QUEUE_DEPTH) return false;
        m_queue.push_back(std::move(job));
        m_cv.notify_one();
    }
    if (util::g_benchstats_enabled.load(std::memory_order_relaxed)) {
        util::g_benchstats.block_decompress_jobs.fetch_add(1, std::memory_order_relaxed);
    }
    return fut.get();
}

void BlockPrefetchQueue::Invalidate()
{
    std::lock_guard lock{m_mutex};
    ++m_generation;
    m_loc = {};
    m_payload.clear();
    m_ready = false;
    m_in_flight = false;
    m_cv.notify_all();
}

void BlockPrefetchQueue::WaitForIdle()
{
    std::unique_lock lock{m_mutex};
    m_cv.wait(lock, [&] { return m_active_workers.load(std::memory_order_relaxed) == 0; });
    m_in_flight = false;
    m_ready = false;
}

void BlockPrefetchQueue::Enqueue(const BlockReadLoc& loc, BlockManager& blockman, const util::SignalInterrupt& interrupt)
{
    if (!loc.IsValid()) return;
    uint64_t generation;
    {
        std::lock_guard lock{m_mutex};
        if (m_in_flight && m_loc.hash == loc.hash && m_loc.pos == loc.pos) return;
        ++m_generation;
        generation = m_generation;
        m_loc = loc;
        m_payload.clear();
        m_ready = false;
        m_in_flight = true;
    }
    m_active_workers.fetch_add(1, std::memory_order_relaxed);
    std::thread{[&blockman, generation, loc, &interrupt, this] {
        struct ActiveWorker {
            BlockPrefetchQueue& queue;
            ~ActiveWorker()
            {
                queue.m_active_workers.fetch_sub(1, std::memory_order_relaxed);
                queue.m_cv.notify_all();
            }
        } worker{*this};
        if (interrupt) return;
        std::vector<uint8_t> payload;
        if (!blockman.ReadBlockFromPayload(payload, loc, /*lowprio=*/true)) return;
        std::lock_guard lock{m_mutex};
        if (generation != m_generation) return;
        m_payload = std::move(payload);
        m_ready = true;
        m_cv.notify_all();
    }}.detach();
}

std::optional<std::vector<uint8_t>> BlockPrefetchQueue::TakeIfReady(const BlockReadLoc& loc)
{
    std::lock_guard lock{m_mutex};
    if (!m_ready || m_loc.hash != loc.hash || m_loc.pos != loc.pos) return std::nullopt;
    m_ready = false;
    m_in_flight = false;
    return std::move(m_payload);
}

void BlockManager::ConfigureDecompressPool(const int workers)
{
    m_decompress_workers = workers;
    if (m_decompress_workers >= 2 && m_block_zstd) {
        m_decompress_pool.Start(m_decompress_workers, &m_block_zstd);
    } else {
        m_decompress_pool.Stop();
    }
}

bool BlockManager::ReadRawBlockFromStored(std::vector<uint8_t>& block,
                                          const BlockDiskHeader& header,
                                          std::span<const uint8_t> stored,
                                          const bool allow_parallel_decompress) const
{
    if (BlockDiskPayloadIsCompressed(header)) {
        const auto decompress_start{SteadyClock::now()};
        bool ok{false};
        const compress::BlockZstd& zstd{BlockDictForDecompress(header.flags, m_block_zstd)};
        // Typed-bucket decompression uses per-bucket dicts inline; parallel pool is mono-dict only.
        const bool use_pool{allow_parallel_decompress
                          && m_decompress_pool.Active()
                          && stored.size() >= kernel::BLOCK_DECOMPRESS_PARALLEL_MIN_SIZE
                          && m_opts.block_zstd_decompress
                          && zstd
                          && (header.flags & BLOCK_SERIALIZATION_FLAG_BUCKET_MASK) == 0
                          && IbdParallelReadsAllowed()};
        if (use_pool && m_decompress_pool.Submit(stored, block, MAX_BLOCK_SERIALIZED_SIZE)) {
            ok = true;
        } else {
            ok = DecompressBlockPayloadImpl(zstd, stored, block, MAX_BLOCK_SERIALIZED_SIZE);
        }
        if (util::g_benchstats_enabled.load(std::memory_order_relaxed)) {
            const auto us{Ticks<std::chrono::microseconds>(SteadyClock::now() - decompress_start)};
            util::g_benchstats.block_decompress_us.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
        }
        if (!ok) return false;
    } else {
        block.assign(stored.begin(), stored.end());
    }
    return block.size() <= MAX_BLOCK_SERIALIZED_SIZE;
}

bool BlockManager::ReadBlockFromPayload(std::vector<uint8_t>& block, const BlockReadLoc& loc, const bool lowprio) const
{
    if (!loc.IsValid()) return false;
    IOPRIO_IDLER(lowprio);
    const uint32_t probe_size{std::min(loc.pos.nPos, BLOCK_SERIALIZATION_HEADER_SIZE)};
    AutoFile filein{OpenBlockFile({loc.pos.nFile, loc.pos.nPos - probe_size}, /*fReadOnly=*/true)};
    if (filein.IsNull()) return false;
    if (lowprio) filein.SetIdlePriority();
    try {
        std::vector<uint8_t> header_bytes(probe_size);
        filein.read(MakeWritableByteSpan(header_bytes));
        BlockDiskHeader header;
        if (!ParseBlockDiskHeader(GetParams(), loc.pos.nPos, header_bytes, header)) return false;
        if (!CompressedBlockReadAllowed(header, m_opts.block_zstd_decompress)) return false;
        std::vector<uint8_t> stored(header.stored_size);
        filein.read(MakeWritableByteSpan(stored));
        return ReadRawBlockFromStored(block, header, stored, /*allow_parallel_decompress=*/true);
    } catch (const std::exception&) {
        return false;
    }
}

bool BlockManager::ReadBlock(CBlock& block, const FlatFilePos& pos, const std::optional<uint256>& expected_hash, const bool lowprio) const
{
    block.SetNull();

    // Open history file to read
    std::vector<uint8_t> block_data;
    if (!ReadRawBlock(block_data, pos, /*lowprio=*/lowprio)) {
        return false;
    }

    // Read block
    try {
        SpanReader{block_data} >> TX_WITH_WITNESS(block);
    } catch (const std::exception& e) {
        LogError("%s: Deserialize or I/O error - %s at %s\n", __func__, e.what(), pos.ToString());
        return false;
    }

    const auto block_hash{block.GetHash()};

    // Check the header
    if (!CheckProofOfWork(block_hash, block.nBits, GetConsensus())) {
        LogError("%s: Errors in block header at %s\n", __func__, pos.ToString());
        return false;
    }

    // Signet only: check block solution
    if (GetConsensus().signet_blocks && !CheckSignetBlockSolution(block, GetConsensus())) {
        LogError("%s: Errors in block solution at %s\n", __func__, pos.ToString());
        return false;
    }

    if (expected_hash && block_hash != *expected_hash) {
        LogError("GetHash() doesn't match index at %s while reading block (%s != %s)",
                 pos.ToString(), block_hash.ToString(), expected_hash->ToString());
        return false;
    }

    return true;
}

bool BlockManager::ReadBlock(CBlock& block, const CBlockIndex& index, const bool lowprio) const
{
    const BlockReadLoc loc{CopyBlockReadLoc(index)};
    if (!loc.IsValid()) {
        return false;
    }
    return ReadBlock(block, loc.pos, loc.hash, /*lowprio=*/lowprio);
}

bool BlockManager::ReadRawBlock(std::vector<uint8_t>& block, const FlatFilePos& pos, const bool lowprio) const
{
    if (pos.nPos < BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE) {
        // If nPos is less than the legacy header size, we can't read the header that precedes the block data.
        // This would cause an unsigned integer underflow when trying to position the file cursor.
        // This can happen after pruning or default constructed positions.
        LogError("%s: OpenBlockFile failed for %s\n", __func__, pos.ToString());
        return false;
    }

    IOPRIO_IDLER(lowprio);

    const auto disk_start{SteadyClock::now()};
    const uint32_t probe_size{std::min(pos.nPos, BLOCK_SERIALIZATION_HEADER_SIZE)};
    AutoFile filein{OpenBlockFile({pos.nFile, pos.nPos - probe_size}, /*fReadOnly=*/true)};
    if (filein.IsNull()) {
        LogError("%s: OpenBlockFile failed for %s\n", __func__, pos.ToString());
        return false;
    }

    if (lowprio) filein.SetIdlePriority();

    try {
        std::vector<uint8_t> header_bytes(probe_size);
        filein.read(MakeWritableByteSpan(header_bytes));

        BlockDiskHeader header;
        if (!ParseBlockDiskHeader(GetParams(), pos.nPos, header_bytes, header)) {
            LogError("%s: Failed to parse block header for %s\n", __func__, pos.ToString());
            return false;
        }

        if (!CompressedBlockReadAllowed(header, m_opts.block_zstd_decompress)) {
            LogError("%s: Compressed block at %s but -blockzstddecompress=0\n", __func__, pos.ToString());
            return false;
        }

        std::vector<uint8_t> stored;
        stored.resize(header.stored_size);
        filein.read(MakeWritableByteSpan(stored));

        if (util::g_benchstats_enabled.load(std::memory_order_relaxed)) {
            const auto us{Ticks<std::chrono::microseconds>(SteadyClock::now() - disk_start)};
            util::g_benchstats.block_read_disk_us.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
        }

        if (!ReadRawBlockFromStored(block, header, stored, /*allow_parallel_decompress=*/true)) {
            if (BlockDiskPayloadIsCompressed(header)) {
                LogError("%s: Failed to decompress block at %s\n", __func__, pos.ToString());
            } else if (block.size() > MAX_BLOCK_SERIALIZED_SIZE) {
                LogError("%s: Block data is larger than maximum deserialization size for %s: %s versus %s\n", __func__,
                         pos.ToString(), block.size(), MAX_BLOCK_SERIALIZED_SIZE);
            }
            return false;
        }
    } catch (const std::exception& e) {
        LogError("%s: Read from block file failed: %s for %s\n", __func__, e.what(), pos.ToString());
        return false;
    }

    return true;
}

bool BlockManager::DecompressBlockPayload(std::span<const uint8_t> compressed, std::vector<uint8_t>& payload) const
{
    return DecompressBlockPayloadImpl(m_block_zstd, compressed, payload, MAX_BLOCK_SERIALIZED_SIZE);
}

FlatFilePos BlockManager::WriteBlock(const CBlock& block, int nHeight)
{
    std::vector<uint8_t> payload;
    VectorWriter{payload, 0} << TX_WITH_WITNESS(block);

    uint8_t flags{0};
    Span<const uint8_t> stored_payload{payload};
    std::vector<uint8_t> compressed;
    const bool use_typed_blocks{compress::g_dict_bootstrap && compress::g_dict_bootstrap->UseTypedBlockFormat()};
    const bool bootstrap_pass1{use_typed_blocks && compress::g_dict_bootstrap->IsPass1InProgress()};
    const bool typed_compress{use_typed_blocks && compress::g_dict_bootstrap->ShouldCompressOnWrite()};
    const bool use_extended_header{m_opts.block_zstd || use_typed_blocks};
    const uint32_t header_size{use_extended_header ? BLOCK_SERIALIZATION_HEADER_SIZE :
                                                     BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE};

    if (bootstrap_pass1) {
        const compress::BlockBucket bucket{compress::ClassifyBlock(nHeight, block, GetParams())};
        flags |= compress::g_dict_bootstrap->BlockFlagsForBucket(bucket);
        compress::g_dict_bootstrap->OnBlockWritten(nHeight, block, payload);
    } else if (typed_compress) {
        const compress::BlockBucket bucket{compress::ClassifyBlock(nHeight, block, GetParams())};
        flags |= compress::g_dict_bootstrap->BlockFlagsForBucket(bucket);
        const compress::DictZstd& dict{compress::g_dict_bootstrap->BlockDict(bucket)};
        if (dict.Compress(payload, compressed, m_opts.block_zstd_level) && compressed.size() < payload.size()) {
            flags |= BLOCK_SERIALIZATION_FLAG_COMPRESSED;
            stored_payload = compressed;
        } else if (!dict && m_block_zstd.Compress(payload, compressed, m_opts.block_zstd_level)
                   && compressed.size() < payload.size()) {
            LogPrintf("Warning: missing typed block dictionary for bucket %s; falling back to monolithic dictionary\n",
                      compress::BlockBucketName(bucket));
            flags |= BLOCK_SERIALIZATION_FLAG_COMPRESSED;
            stored_payload = compressed;
        } else if (!dict) {
            LogPrintf("Warning: missing typed block dictionary for bucket %s; writing uncompressed payload\n",
                      compress::BlockBucketName(bucket));
        }
        compress::g_dict_bootstrap->OnBlockStored(bucket, payload.size(), stored_payload.size());
    } else if (use_extended_header && m_block_zstd && (!compress::g_dict_bootstrap || compress::g_dict_bootstrap->ShouldCompressOnWrite())) {
        if (m_block_zstd.Compress(payload, compressed, m_opts.block_zstd_level) &&
            compressed.size() < payload.size()) {
            flags |= BLOCK_SERIALIZATION_FLAG_COMPRESSED;
            stored_payload = compressed;
        }
    }

    const unsigned int stored_size{static_cast<unsigned int>(stored_payload.size())};
    FlatFilePos pos{FindNextBlockPos(stored_size + header_size, nHeight, block.GetBlockTime())};
    if (pos.IsNull()) {
        LogError("FindNextBlockPos failed for %s while writing block", pos.ToString());
        return FlatFilePos();
    }
    AutoFile file{OpenBlockFile(pos, /*fReadOnly=*/false)};
    if (file.IsNull()) {
        LogError("OpenBlockFile failed for %s while writing block", pos.ToString());
        m_opts.notifications.fatalError(_("Failed to write block."));
        return FlatFilePos();
    }
    {
        BufferedWriter fileout{file};

        fileout << GetParams().MessageStart();
        if (use_extended_header) {
            fileout << flags << stored_size;
        } else {
            fileout << stored_size;
        }
        pos.nPos += header_size;
        fileout.write(AsBytes(stored_payload));
    }

    if (file.fclose() != 0) {
        LogError("Failed to close block file %s: %s", pos.ToString(), SysErrorString(errno));
        m_opts.notifications.fatalError(_("Failed to close file when writing block."));
        return FlatFilePos();
    }

    return pos;
}

static auto InitBlocksdirXorKey(const BlockManager::Options& opts)
{
    // Bytes are serialized without length indicator, so this is also the exact
    // size of the XOR-key file.
    std::array<std::byte, 8> xor_key{};

    // Consider this to be the first run if the blocksdir contains only hidden
    // files (those which start with a .). Checking for a fully-empty dir would
    // be too aggressive as a .lock file may have already been written.
    bool first_run = true;
    for (const auto& entry : fs::directory_iterator(opts.blocks_dir)) {
        const std::string path = fs::PathToString(entry.path().filename());
        if (!entry.is_regular_file() || !path.starts_with('.')) {
            first_run = false;
            break;
        }
    }

    if (opts.use_xor && first_run) {
        xor_key = Obfuscation::DEFAULT_KEY_BYTES;
    }

    const fs::path xor_key_path{opts.blocks_dir / "xor.dat"};
    if (fs::exists(xor_key_path)) {
        // A pre-existing xor key file has priority.
        AutoFile xor_key_file{fsbridge::fopen(xor_key_path, "rb")};
        xor_key_file >> xor_key;
    } else {
        // Create initial or missing xor key file
        AutoFile xor_key_file{fsbridge::fopen(xor_key_path,
#if 0
            "wb" // Temporary workaround for https://github.com/bitcoin/bitcoin/issues/30210
#else
            "wbx"
#endif
        )};
        xor_key_file << xor_key;
        if (xor_key_file.fclose() != 0) {
            throw std::runtime_error{strprintf("Error closing XOR key file %s: %s",
                                               fs::PathToString(xor_key_path),
                                               SysErrorString(errno))};
        }
    }
    // If the user disabled the key, it must be zero.
    if (!opts.use_xor && xor_key != decltype(xor_key){}) {
        throw std::runtime_error{
            strprintf("The blocksdir XOR-key can not be disabled when a random key was already stored! "
                      "Stored key: '%s', stored path: '%s'.",
                      HexStr(xor_key), fs::PathToString(xor_key_path)),
        };
    }
    LogInfo("Using obfuscation key for blocksdir *.dat files (%s): '%s'\n", fs::PathToString(opts.blocks_dir), HexStr(xor_key));
    return Obfuscation{xor_key};
}

void BlockManager::ResizeBlockTreeCache(size_t new_cache_size)
{
    AssertLockHeld(::cs_main);
    if (m_opts.block_tree_db_params.memory_only) return;
    if (m_block_tree_cache_bytes == new_cache_size) return;

    DBParams params{m_opts.block_tree_db_params};
    params.cache_bytes = new_cache_size;
    params.wipe_data = false;
    m_block_tree_db = std::make_unique<BlockTreeDB>(params);
    m_block_tree_cache_bytes = new_cache_size;
    LogPrintf("Resized block tree cache to %.1f MiB\n", new_cache_size * (1.0 / 1024 / 1024));
}

BlockManager::BlockManager(const util::SignalInterrupt& interrupt, Options opts)
    : m_prune_mode{opts.prune_target > 0},
      m_xor_key{InitBlocksdirXorKey(opts)},
      m_block_zstd{InitBlockZstd(opts)},
      m_opts{std::move(opts)},
      m_block_tree_cache_bytes{m_opts.block_tree_db_params.cache_bytes},
      m_block_file_seq{FlatFileSeq{m_opts.blocks_dir, "blk", m_opts.fast_prune ? 0x4000 /* 16kB */ : BLOCKFILE_CHUNK_SIZE}},
      m_undo_file_seq{FlatFileSeq{m_opts.blocks_dir, "rev", UNDOFILE_CHUNK_SIZE}},
      m_interrupt{interrupt}
{
    m_block_tree_db = std::make_unique<BlockTreeDB>(m_opts.block_tree_db_params);

    if (m_opts.block_tree_db_params.wipe_data) {
        m_block_tree_db->WriteReindexing(true);
        m_blockfiles_indexed = false;
        // If we're reindexing in prune mode, wipe away unusable block files and all undo data files
        if (m_prune_mode) {
            CleanupBlockRevFiles();
        }
    }

    if (m_opts.block_zstd && !m_block_zstd) {
        LogWarning("Block zstd compression is enabled (-blockzstd=1) but no dictionary is loaded; "
                   "new blocks will be written with extended headers without compression\n");
    }
    if (m_opts.block_zstd_decompress && !m_block_zstd) {
        LogWarning("Block zstd decompression is enabled (-blockzstddecompress=1) but no dictionary is loaded; "
                   "compressed blocks on disk cannot be read\n");
    }
    m_decompress_workers = m_opts.block_decompress_workers;
    ConfigureDecompressPool(m_decompress_workers);
}

class ImportingNow
{
    std::atomic<bool>& m_importing;

public:
    ImportingNow(std::atomic<bool>& importing) : m_importing{importing}
    {
        assert(m_importing == false);
        m_importing = true;
    }
    ~ImportingNow()
    {
        assert(m_importing == true);
        m_importing = false;
    }
};

void ImportBlocks(ChainstateManager& chainman, std::span<const fs::path> import_paths)
{
    ImportingNow imp{chainman.m_blockman.m_importing};

    // -reindex
    if (!chainman.m_blockman.m_blockfiles_indexed) {
        int nFile = 0;
        // Map of disk positions for blocks with unknown parent (only used for reindex);
        // parent hash -> child disk position, multiple children can have the same parent.
        std::multimap<uint256, OutOfOrderBlockDiskEntry> blocks_with_unknown_parent;
        while (true) {
            FlatFilePos pos(nFile, 0);
            if (!fs::exists(chainman.m_blockman.GetBlockPosFilename(pos))) {
                break; // No block files left to reindex
            }
            AutoFile file{chainman.m_blockman.OpenBlockFile(pos, true)};
            if (file.IsNull()) {
                break; // This error is logged in OpenBlockFile
            }
            LogPrintf("Reindexing block file blk%05u.dat...\n", (unsigned int)nFile);
            chainman.LoadExternalBlockFile(file, &pos, &blocks_with_unknown_parent);
            if (chainman.m_interrupt) {
                LogPrintf("Interrupt requested. Exit %s\n", __func__);
                return;
            }
            nFile++;
        }
        WITH_LOCK(::cs_main, chainman.m_blockman.m_block_tree_db->WriteReindexing(false));
        chainman.m_blockman.m_blockfiles_indexed = true;
        LogPrintf("Reindexing finished\n");
        // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
        chainman.ActiveChainstate().LoadGenesisBlock();
    }

    // -loadblock=
    for (const fs::path& path : import_paths) {
        AutoFile file{fsbridge::fopen(path, "rb")};
        if (!file.IsNull()) {
            LogPrintf("Importing blocks file %s...\n", fs::PathToString(path));
            chainman.LoadExternalBlockFile(file);
            if (chainman.m_interrupt) {
                LogPrintf("Interrupt requested. Exit %s\n", __func__);
                return;
            }
        } else {
            LogWarning("Could not open blocks file %s", fs::PathToString(path));
        }
    }

    // scan for better chains in the block chain database, that are not yet connected in the active best chain

    // We can't hold cs_main during ActivateBestChain even though we're accessing
    // the chainman unique_ptrs since ABC requires us not to be holding cs_main, so retrieve
    // the relevant pointers before the ABC call.
    for (Chainstate* chainstate : WITH_LOCK(::cs_main, return chainman.GetAll())) {
        BlockValidationState state;
        if (!chainstate->ActivateBestChain(state, nullptr)) {
            chainman.GetNotifications().fatalError(strprintf(_("Failed to connect best block (%s)."), state.ToString()));
            return;
        }
    }
    if (compress::g_dict_bootstrap) {
        compress::g_dict_bootstrap->OnReindexComplete();
    }
    // End scope of ImportingNow
}

std::ostream& operator<<(std::ostream& os, const BlockfileType& type) {
    switch(type) {
        case BlockfileType::NORMAL: os << "normal"; break;
        case BlockfileType::ASSUMED: os << "assumed"; break;
        default: os.setstate(std::ios_base::failbit);
    }
    return os;
}

std::ostream& operator<<(std::ostream& os, const BlockfileCursor& cursor) {
    os << strprintf("BlockfileCursor(file_num=%d, undo_height=%d)", cursor.file_num, cursor.undo_height);
    return os;
}
} // namespace node

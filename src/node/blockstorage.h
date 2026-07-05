// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCKSTORAGE_H
#define BITCOIN_NODE_BLOCKSTORAGE_H

#include <attributes.h>
#include <chain.h>
#include <compress/zstd.h>
#include <node/blockfile_format.h>
#include <dbwrapper.h>
#include <flatfile.h>
#include <kernel/blockmanager_opts.h>
#include <kernel/chainparams.h>
#include <kernel/cs_main.h>
#include <kernel/messagestartchars.h>
#include <primitives/block.h>
#include <streams.h>
#include <sync.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/hasher.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class BlockValidationState;
class CBlockUndo;
class Chainstate;
class ChainstateManager;
namespace Consensus {
struct Params;
}
namespace node {
struct PruneLockInfo;
};
namespace util {
class SignalInterrupt;
} // namespace util

namespace node {
struct BlockIndexWriteBatch;
class BlockManager;

} // namespace node

namespace kernel {
/** Access to the block database (blocks/index/) */
class BlockTreeDB : public CDBWrapper
{
public:
    using CDBWrapper::CDBWrapper;
    bool WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*>>& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo, const std::unordered_map<std::string, node::PruneLockInfo>& prune_locks);
    bool WriteBatchSync(const node::BlockIndexWriteBatch& batch);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo& info);
    bool ReadLastBlockFile(int& nFile);
    bool WriteReindexing(bool fReindexing);
    void ReadReindexing(bool& fReindexing);
    bool WritePruneLock(const std::string& name, const node::PruneLockInfo&);
    bool DeletePruneLock(const std::string& name);
    bool WriteFlag(const std::string& name, bool fValue);
    bool ReadFlag(const std::string& name, bool& fValue);
    bool LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex, const util::SignalInterrupt& interrupt)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool LoadPruneLocks(std::unordered_map<std::string, node::PruneLockInfo>& prune_locks, const util::SignalInterrupt& interrupt);
};
} // namespace kernel

namespace node {
using kernel::BlockTreeDB;

/** The pre-allocation chunk size for blk?????.dat files (since 0.8) */
static const unsigned int BLOCKFILE_CHUNK_SIZE = 0x1000000; // 16 MiB
/** The pre-allocation chunk size for rev?????.dat files (since 0.8) */
static const unsigned int UNDOFILE_CHUNK_SIZE = 0x100000; // 1 MiB
/** The maximum size of a blk?????.dat file (since 0.8) */
static const unsigned int MAX_BLOCKFILE_SIZE = 0x8000000; // 128 MiB

/** Total overhead when writing undo data: header (8 bytes) plus checksum (32 bytes) */
static constexpr uint32_t UNDO_DATA_DISK_OVERHEAD{BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE + uint256::size()};

// Because validation code takes pointers to the map's CBlockIndex objects, if
// we ever switch to another associative container, we need to either use a
// container that has stable addressing (true of all std associative
// containers), or make the key a `std::unique_ptr<CBlockIndex>`
using BlockMap = std::unordered_map<uint256, CBlockIndex, BlockHasher>;

struct CBlockIndexWorkComparator {
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const;
};

struct CBlockIndexHeightOnlyComparator {
    /* Only compares the height of two block indices, doesn't try to tie-break */
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const;
};

struct PruneLockInfo {
    std::string desc; //! Arbitrary human-readable description of the lock purpose
    uint64_t height_first{std::numeric_limits<uint64_t>::max()}; //! Height of earliest block that should be kept and not pruned
    uint64_t height_last{std::numeric_limits<uint64_t>::max()}; //! Height of latest block that should be kept and not pruned
    bool temporary{true};

    SERIALIZE_METHODS(PruneLockInfo, obj)
    {
        READWRITE(obj.desc);
        READWRITE(VARINT(obj.height_first));
        READWRITE(VARINT(obj.height_last));
    }
};

/** Snapshot of block index fields needed to read a block from disk without holding cs_main. */
struct BlockReadLoc {
    FlatFilePos pos;
    uint256 hash;
    bool have_data{false};

    bool IsValid() const
    {
        return have_data && pos.nPos >= BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE;
    }
};

class BlockManager;

/** Worker pool for zstd block payload decompression during IBD reads. */
class BlockDecompressPool
{
public:
    BlockDecompressPool() = default;
    ~BlockDecompressPool() { Stop(); }

    void Start(int num_workers, const compress::BlockZstd* zstd);
    void Stop();

    //! Returns false when the pool is stopped or the job queue is full (caller runs inline).
    bool Submit(std::span<const uint8_t> compressed, std::vector<uint8_t>& payload, size_t max_output) const;

    bool Active() const { return m_num_workers >= 2; }

private:
    struct Job {
        std::span<const uint8_t> compressed;
        std::vector<uint8_t>* payload{nullptr};
        size_t max_output{0};
        std::promise<bool> done;
    };

    void WorkerLoop();

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    mutable std::deque<std::unique_ptr<Job>> m_queue;
    std::vector<std::thread> m_workers;
    const compress::BlockZstd* m_zstd{nullptr};
    std::atomic<bool> m_stop{false};
    int m_num_workers{0};
    static constexpr size_t MAX_QUEUE_DEPTH{4};
};

/** Depth-1 async block read + decompress pipeline for ConnectTip. */
class BlockPrefetchQueue
{
public:
    void Invalidate();
    void Enqueue(const BlockReadLoc& loc, BlockManager& blockman, const util::SignalInterrupt& interrupt);
    //! Take a ready prefetched payload when loc matches; otherwise returns nullopt.
    std::optional<std::vector<uint8_t>> TakeIfReady(const BlockReadLoc& loc);
    void WaitForIdle();

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    BlockReadLoc m_loc;
    std::vector<uint8_t> m_payload;
    std::atomic<int> m_active_workers{0};
    bool m_ready{false};
    bool m_in_flight{false};
    uint64_t m_generation{0};
};

/** Snapshot of block index fields needed to read undo data from disk without holding cs_main. */
struct UndoReadLoc {
    FlatFilePos pos;
    uint256 prev_block_hash;
    bool have_undo{false};

    bool IsValid() const
    {
        return have_undo && !pos.IsNull();
    }
};

/** Immutable block index write batch collected under cs_main for LMDB write outside the lock. */
struct BlockIndexWriteBatch {
    std::vector<std::pair<int, CBlockFileInfo>> file_info;
    int last_file{0};
    std::vector<std::pair<uint256, CDiskBlockIndex>> block_indices;
    std::unordered_map<std::string, PruneLockInfo> prune_locks;

    bool empty() const { return file_info.empty() && block_indices.empty(); }
};

enum BlockfileType {
    // Values used as array indexes - do not change carelessly.
    NORMAL = 0,
    ASSUMED = 1,
    NUM_TYPES = 2,
};

std::ostream& operator<<(std::ostream& os, const BlockfileType& type);

struct BlockfileCursor {
    // The latest blockfile number.
    int file_num{0};

    // Track the height of the highest block in file_num whose undo
    // data has been written. Block data is written to block files in download
    // order, but is written to undo files in validation order, which is
    // usually in order by height. To avoid wasting disk space, undo files will
    // be trimmed whenever the corresponding block file is finalized and
    // the height of the highest block written to the block file equals the
    // height of the highest block written to the undo file. This is a
    // heuristic and can sometimes preemptively trim undo files that will write
    // more data later, and sometimes fail to trim undo files that can't have
    // more data written later.
    int undo_height{0};
};

std::ostream& operator<<(std::ostream& os, const BlockfileCursor& cursor);


/**
 * Maintains a tree of blocks (stored in `m_block_index`) which is consulted
 * to determine where the most-work tip is.
 *
 * This data is used mostly in `Chainstate` - information about, e.g.,
 * candidate tips is not maintained here.
 */
class BlockManager
{
    friend Chainstate;
    friend ChainstateManager;

private:
    const CChainParams& GetParams() const { return m_opts.chainparams; }
    const Consensus::Params& GetConsensus() const { return m_opts.chainparams.GetConsensus(); }
    /**
     * Load the blocktree off disk and into memory. Populate certain metadata
     * per index entry (nStatus, nChainWork, nTimeMax, etc.) as well as peripheral
     * collections like m_dirty_blockindex.
     */
    bool LoadBlockIndex(const std::optional<uint256>& snapshot_blockhash)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Return false if block file or undo file flushing fails. */
    [[nodiscard]] bool FlushBlockFile(int blockfile_num, bool fFinalize, bool finalize_undo);

    /** Return false if undo file flushing fails. */
    [[nodiscard]] bool FlushUndoFile(int block_file, bool finalize = false);

    /**
     * Helper function performing various preparations before a block can be saved to disk:
     * Returns the correct position for the block to be saved, which may be in the current or a new
     * block file depending on nAddSize. May flush the previous blockfile to disk if full, updates
     * blockfile info, and checks if there is enough disk space to save the block.
     *
     * The nAddSize argument passed to this function should include not just the size of the serialized CBlock, but also the size of
     * separator fields (BLOCK_SERIALIZATION_HEADER_SIZE).
     */
    [[nodiscard]] FlatFilePos FindNextBlockPos(unsigned int nAddSize, unsigned int nHeight, uint64_t nTime);
    [[nodiscard]] bool FlushChainstateBlockFile(int tip_height);
    bool FindUndoPos(BlockValidationState& state, int nFile, FlatFilePos& pos, unsigned int nAddSize);

    AutoFile OpenUndoFile(const FlatFilePos& pos, bool fReadOnly = false) const;

    bool DoPruneLocksForbidPruning(const CBlockFileInfo& block_file_info) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /* Calculate the block/rev files to delete based on height specified by user with RPC command pruneblockchain */
    void FindFilesToPruneManual(
        std::set<int>& setFilesToPrune,
        int nManualPruneHeight,
        const Chainstate& chain,
        ChainstateManager& chainman);

    /**
     * Prune block and undo files (blk???.dat and rev???.dat) so that the disk space used is less than a user-defined target.
     * The user sets the target (in MB) on the command line or in config file.  This will be run on startup and whenever new
     * space is allocated in a block or undo file, staying below the target. Changing back to unpruned requires a reindex
     * (which in this case means the blockchain must be re-downloaded.)
     *
     * Pruning functions are called from FlushStateToDisk when the m_check_for_pruning flag has been set.
     * Block and undo files are deleted in lock-step (when blk00003.dat is deleted, so is rev00003.dat.)
     * Pruning cannot take place until the longest chain is at least a certain length (CChainParams::nPruneAfterHeight).
     * Pruning will never delete a block within a defined distance (currently 288) from the active chain's tip.
     * The block index is updated by unsetting HAVE_DATA and HAVE_UNDO for any blocks that were stored in the deleted files.
     * A db flag records the fact that at least some block files have been pruned.
     *
     * @param[out]   setFilesToPrune   The set of file indices that can be unlinked will be returned
     * @param        last_prune        The last height we're able to prune, according to the prune locks
     */
    void FindFilesToPrune(
        std::set<int>& setFilesToPrune,
        int last_prune,
        const Chainstate& chain,
        ChainstateManager& chainman);

    RecursiveMutex cs_LastBlockFile;
    std::vector<CBlockFileInfo> m_blockfile_info;

    //! Since assumedvalid chainstates may be syncing a range of the chain that is very
    //! far away from the normal/background validation process, we should segment blockfiles
    //! for assumed chainstates. Otherwise, we might have wildly different height ranges
    //! mixed into the same block files, which would impair our ability to prune
    //! effectively.
    //!
    //! This data structure maintains separate blockfile number cursors for each
    //! BlockfileType. The ASSUMED state is initialized, when necessary, in FindNextBlockPos().
    //!
    //! The first element is the NORMAL cursor, second is ASSUMED.
    std::array<std::optional<BlockfileCursor>, BlockfileType::NUM_TYPES>
        m_blockfile_cursors GUARDED_BY(cs_LastBlockFile) = {
            BlockfileCursor{},
            std::nullopt,
    };
    int MaxBlockfileNum() const EXCLUSIVE_LOCKS_REQUIRED(cs_LastBlockFile)
    {
        static const BlockfileCursor empty_cursor;
        const auto& normal = m_blockfile_cursors[BlockfileType::NORMAL].value_or(empty_cursor);
        const auto& assumed = m_blockfile_cursors[BlockfileType::ASSUMED].value_or(empty_cursor);
        return std::max(normal.file_num, assumed.file_num);
    }

    /** Global flag to indicate we should check to see if there are
     *  block/undo files that should be deleted.  Set on startup
     *  or if we allocate more file space when we're in prune mode
     */
    bool m_check_for_pruning = false;

    const bool m_prune_mode;

    const Obfuscation m_xor_key;

    compress::BlockZstd m_block_zstd;
    mutable BlockDecompressPool m_decompress_pool;
    BlockPrefetchQueue m_prefetch_queue;
    int m_decompress_workers{0};
    std::atomic<bool> m_ibd_parallel_reads{false};

    /** Dirty block index entries. */
    std::set<CBlockIndex*> m_dirty_blockindex;

    /** Dirty block file entries. */
    std::set<int> m_dirty_fileinfo;

public:
    /**
     * Map from external index name to oldest block that must not be pruned.
     *
     * @note Internally, only blocks before height (height_first - PRUNE_LOCK_BUFFER - 1) and
     * after height (height_last + PRUNE_LOCK_BUFFER) will be pruned, but callers should
     * avoid assuming any particular buffer size.
     */
    std::unordered_map<std::string, PruneLockInfo> m_prune_locks GUARDED_BY(::cs_main);

private:
    BlockfileType BlockfileTypeForHeight(int height);

    const kernel::BlockManagerOpts m_opts;
    size_t m_block_tree_cache_bytes;

    const FlatFileSeq m_block_file_seq;
    const FlatFileSeq m_undo_file_seq;

public:
    using Options = kernel::BlockManagerOpts;

    explicit BlockManager(const util::SignalInterrupt& interrupt, Options opts);

    const util::SignalInterrupt& m_interrupt;
    std::atomic<bool> m_importing{false};

    /**
     * Whether all blockfiles have been added to the block tree database.
     * Normally true, but set to false when a reindex is requested and the
     * database is wiped. The value is persisted in the database across restarts
     * and will be false until reindexing completes.
     */
    std::atomic_bool m_blockfiles_indexed{true};

    BlockMap m_block_index GUARDED_BY(cs_main);

    /**
     * The height of the base block of an assumeutxo snapshot, if one is in use.
     *
     * This controls how blockfiles are segmented by chainstate type to avoid
     * comingling different height regions of the chain when an assumedvalid chainstate
     * is in use. If heights are drastically different in the same blockfile, pruning
     * suffers.
     *
     * This is set during ActivateSnapshot() or upon LoadBlockIndex() if a snapshot
     * had been previously loaded. After the snapshot is validated, this is unset to
     * restore normal LoadBlockIndex behavior.
     */
    std::optional<int> m_snapshot_height;

    std::vector<CBlockIndex*> GetAllBlockIndices() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
     * Pruned nodes may have entries where B is missing data.
     */
    std::multimap<CBlockIndex*, CBlockIndex*> m_blocks_unlinked;

    //! Serializes block-index collect→LMDB-write across chainstates (shared m_block_tree_db).
    Mutex m_cs_block_index_write;

    // LMDB writes also require m_cs_block_index_write (see WriteBlockIndexBatch).
    std::unique_ptr<BlockTreeDB> m_block_tree_db GUARDED_BY(::cs_main);

    bool WriteBlockIndexDB();
    //! Copy dirty block index state without clearing dirty flags (caller must hold cs_main).
    BlockIndexWriteBatch PrepareBlockIndexWriteBatch() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Clear dirty flags for a batch whose LMDB write succeeded (caller must hold cs_main).
    void CommitBlockIndexWriteBatch(const BlockIndexWriteBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Write a prepared batch to LMDB (requires m_cs_block_index_write; no cs_main).
    bool WriteBlockIndexBatch(const BlockIndexWriteBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(m_cs_block_index_write);
    bool LoadBlockIndexDB(const std::optional<uint256>& snapshot_blockhash)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Remove any pruned block & undo files that are still on disk.
     * This could happen on some systems if the file was still being read while unlinked,
     * or if we crash before unlinking.
     */
    void ScanAndUnlinkAlreadyPrunedFiles() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    CBlockIndex* AddToBlockIndex(const CBlockHeader& block, CBlockIndex*& best_header) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /** Create a new block index entry for a given block hash */
    CBlockIndex* InsertBlockIndex(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Mark one block file as pruned (modify associated database entries)
    void PruneOneBlockFile(const int fileNumber) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    CBlockIndex* LookupBlockIndex(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    const CBlockIndex* LookupBlockIndex(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Get block file info entry for one block file */
    CBlockFileInfo* GetBlockFileInfo(size_t n);

    bool WriteBlockUndo(const CBlockUndo& blockundo, BlockValidationState& state, CBlockIndex& block)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Store block on disk and update block file statistics.
     *
     * @param[in]  block        the block to be stored
     * @param[in]  nHeight      the height of the block
     *
     * @returns in case of success, the position to which the block was written to
     *          in case of an error, an empty FlatFilePos
     */
    FlatFilePos WriteBlock(const CBlock& block, int nHeight);

    /** Update blockfile info while processing a block during reindex. The block must be available on disk.
     *
     * @param[in]  block        the block being processed
     * @param[in]  nHeight      the height of the block
     * @param[in]  pos          the position of the serialized CBlock on disk
     * @param[in]  on_disk_payload_size  Optional on-disk payload size when it differs from the
     *                                   serialized size (e.g. zstd-compressed during reindex).
     */
    void UpdateBlockInfo(const CBlock& block, unsigned int nHeight, const FlatFilePos& pos,
                         std::optional<unsigned int> on_disk_payload_size = std::nullopt);

    /** Whether reading zstd-compressed block payloads is allowed (-blockzstddecompress). */
    [[nodiscard]] bool AllowsBlockZstdDecompress() const { return m_opts.block_zstd_decompress; }

    /** Whether running in -prune mode. */
    [[nodiscard]] bool IsPruneMode() const { return m_prune_mode; }

    /** Attempt to stay below this number of bytes of block files. */
    [[nodiscard]] uint64_t GetPruneTarget() const { return m_opts.prune_target; }
    [[nodiscard]] uint64_t GetPruneTargetForChainstate(const Chainstate& chain, ChainstateManager& chainman) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    static constexpr auto PRUNE_TARGET_MANUAL{std::numeric_limits<uint64_t>::max()};

    [[nodiscard]] bool LoadingBlocks() const { return m_importing || !m_blockfiles_indexed; }

    /** Calculate the amount of disk space the block & undo files currently use */
    uint64_t CalculateCurrentUsage();

    //! Returns last CBlockIndex* that is a checkpoint
    const CBlockIndex* GetLastCheckpoint(const CCheckpointData& data) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Check if all blocks in the [upper_block, lower_block] range have data available.
    //! The caller is responsible for ensuring that lower_block is an ancestor of upper_block
    //! (part of the same chain).
    bool CheckBlockDataAvailability(const CBlockIndex& upper_block LIFETIMEBOUND, const CBlockIndex& lower_block LIFETIMEBOUND) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * @brief Returns the earliest block with specified `status_mask` flags set after
     * the latest block _not_ having those flags.
     *
     * This function starts from `upper_block`, which must have all
     * `status_mask` flags set, and iterates backwards through its ancestors. It
     * continues as long as each block has all `status_mask` flags set, until
     * reaching the oldest ancestor or `lower_block`.
     *
     * @pre `upper_block` must have all `status_mask` flags set.
     * @pre `lower_block` must be null or an ancestor of `upper_block`
     *
     * @param upper_block The starting block for the search, which must have all
     *                    `status_mask` flags set.
     * @param status_mask Bitmask specifying required status flags.
     * @param lower_block The earliest possible block to return. If null, the
     *                    search can extend to the genesis block.
     *
     * @return A non-null pointer to the earliest block between `upper_block`
     *         and `lower_block`, inclusive, such that every block between the
     *         returned block and `upper_block` has `status_mask` flags set.
     */
    const CBlockIndex* GetFirstBlock(
        const CBlockIndex& upper_block LIFETIMEBOUND,
        uint32_t status_mask,
        const CBlockIndex* lower_block = nullptr
    ) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** True if any block files have ever been pruned. */
    bool m_have_pruned = false;

    //! Check whether the block associated with this index entry is pruned or not.
    bool IsBlockPruned(const CBlockIndex& block) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    bool PruneLockExists(const std::string& name) const SHARED_LOCKS_REQUIRED(::cs_main);
    //! Create or update a prune lock identified by its name
    bool UpdatePruneLock(const std::string& name, const PruneLockInfo& lock_info, bool sync=false) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool DeletePruneLock(const std::string& name) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Open a block file (blk?????.dat) */
    AutoFile OpenBlockFile(const FlatFilePos& pos, bool fReadOnly) const;

    /** Translation to a filesystem path */
    fs::path GetBlockPosFilename(const FlatFilePos& pos) const;

    /**
     *  Actually unlink the specified files
     */
    void UnlinkPrunedFiles(const std::set<int>& setFilesToPrune) const;

    /** Functions for disk access for blocks */
    bool ReadBlock(CBlock& block, const FlatFilePos& pos, const std::optional<uint256>& expected_hash = {}, bool lowprio = false) const;
    bool ReadBlock(CBlock& block, const CBlockIndex& index, bool lowprio = false) const;
    bool ReadRawBlock(std::vector<uint8_t>& block, const FlatFilePos& pos, bool lowprio = false) const;

    //! Copy block read metadata (caller must hold cs_main).
    BlockReadLoc CopyBlockReadLocAssumingLockHeld(const CBlockIndex& index) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Copy block read metadata under a brief cs_main lock.
    BlockReadLoc CopyBlockReadLoc(const CBlockIndex& index) const;
    //! Copy undo read metadata (caller must hold cs_main).
    UndoReadLoc CopyUndoReadLocAssumingLockHeld(const CBlockIndex& index) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    //! Copy undo read metadata under a brief cs_main lock.
    UndoReadLoc CopyUndoReadLoc(const CBlockIndex& index) const;

    /** Decompress a zstd-compressed block payload. Returns false if unavailable or invalid. */
    bool DecompressBlockPayload(std::span<const uint8_t> compressed, std::vector<uint8_t>& payload) const;

    void StopDecompressPool() { m_decompress_pool.Stop(); }
    BlockPrefetchQueue& PrefetchQueue() { return m_prefetch_queue; }

    void SetIbdParallelReadsAllowed(bool allowed) { m_ibd_parallel_reads.store(allowed, std::memory_order_relaxed); }
    bool IbdParallelReadsAllowed() const { return m_ibd_parallel_reads.load(std::memory_order_relaxed); }

    void ConfigureDecompressPool(int workers);

    bool ReadRawBlockFromStored(std::vector<uint8_t>& block,
                                const BlockDiskHeader& header,
                                std::span<const uint8_t> stored,
                                bool allow_parallel_decompress) const;

    bool ReadBlockFromPayload(std::vector<uint8_t>& block, const BlockReadLoc& loc, bool lowprio = false) const;

    bool ReadBlockUndo(CBlockUndo& blockundo, const CBlockIndex& index) const;
    bool ReadBlockUndo(CBlockUndo& blockundo, const UndoReadLoc& loc) const;

    void CleanupBlockRevFiles() const;

    //! Current block index LMDB reader pool budget (bytes).
    size_t GetBlockTreeCacheSize() const { return m_block_tree_cache_bytes; }

    //! Reopen the block index LMDB environment with a new reader pool budget.
    void ResizeBlockTreeCache(size_t new_cache_size) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};

// Calls ActivateBestChain() even if no blocks are imported.
void ImportBlocks(ChainstateManager& chainman, std::span<const fs::path> import_paths);
} // namespace node

#endif // BITCOIN_NODE_BLOCKSTORAGE_H

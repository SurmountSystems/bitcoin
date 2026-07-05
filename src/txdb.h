// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include <coins.h>
#include <compress/zstd.h>
#include <dbwrapper.h>
#include <kernel/cs_main.h>
#include <sync.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class COutPoint;
class uint256;

struct PendingCoinWrite;

//! -dbbatchsize default (bytes)
static const int64_t nDefaultDbBatchSize = 64 << 20;

static constexpr bool DEFAULT_UTXO_ZSTD{true};
static constexpr int DEFAULT_UTXO_ZSTD_LEVEL{20};
//! Minimum dirty UTXO count before parallel pre-encode is used.
static constexpr size_t UTXO_ENCODE_PARALLEL_THRESHOLD{256};
//! -utxoencodepar default (0 = auto from -par, 1 = serial encode only).
static constexpr int DEFAULT_UTXO_ENCODE_PAR{0};
static constexpr int MAX_UTXO_ENCODE_PAR{8};
static constexpr bool DEFAULT_FLUSH_SNAPSHOT{true};
//! -coinprefetchpar default (0 = auto from -par, 1 = serial prefetch only).
static constexpr int DEFAULT_COIN_PREFETCH_PAR{0};
static constexpr int MAX_COIN_PREFETCH_PAR{8};
//! Minimum unique uncached prevouts before parallel coin prefetch runs.
static constexpr size_t COIN_PREFETCH_PARALLEL_THRESHOLD{64};

//! User-controlled performance and debug options.
struct CoinsViewOptions {
    //! Maximum database write batch size in bytes.
    size_t batch_write_bytes = nDefaultDbBatchSize;
    //! Sync the final WriteBatch to disk (mdb_env_sync). Set by FlushStateToDisk on ALWAYS.
    bool sync_final_batch = false;
    //! Sync every chainstate WriteBatch (-lmdbsync, debug/paranoid mode).
    bool lmdbsync = false;
    //! If non-zero, randomly exit when the database is flushed with (1/ratio)
    //! probability.
    int simulate_crash_ratio = 0;
    //! Enable zstd dictionary compression for new UTXO writes.
    bool utxo_zstd{DEFAULT_UTXO_ZSTD};
    //! zstd compression level for UTXO storage.
    int utxo_zstd_level{DEFAULT_UTXO_ZSTD_LEVEL};
    //! Optional override path for the UTXO zstd dictionary.
    std::optional<fs::path> utxo_zstd_dict_path;
    //! Parallel UTXO encode worker threads (0 = serial only; >0 from -utxoencodepar resolution).
    int utxo_encode_workers{0};
    //! Parallel LMDB coin prefetch worker threads (0 = serial only; >0 from -coinprefetchpar).
    int coin_prefetch_workers{0};
    //! Snapshot dirty UTXOs and release cs_main during LMDB writes (0 = legacy in-lock path).
    bool flush_snapshot{DEFAULT_FLUSH_SNAPSHOT};
};

/** CCoinsView backed by the coin database (chainstate/)
 * Cursor requires FlushStateToDisk for consistency.
 */
class CCoinsViewDB final : public CCoinsView
{
protected:
    DBParams m_db_params;
    CoinsViewOptions m_options;
    std::unique_ptr<CDBWrapper> m_db;
    compress::UtxoZstd m_utxo_zstd;

    bool ReadCoinValue(const COutPoint& outpoint, Coin& coin) const;
    void WriteCoinValue(CDBBatch& batch, const COutPoint& outpoint, const Coin& coin);
    bool WritePendingToDisk(const uint256& hashBlock, std::vector<PendingCoinWrite>& pending, size_t count, size_t changed);

public:
    //! Thread-safe LMDB read for prefetch workers (no cache mutation).
    bool ReadCoinValueForPrefetch(const COutPoint& outpoint, Coin& coin) const;
    bool ReadStoredCoinBytesForPrefetch(const COutPoint& outpoint, std::vector<uint8_t>& bytes) const;
    explicit CCoinsViewDB(DBParams db_params, CoinsViewOptions options);

    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint &outpoint) const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    bool BatchWrite(CoinsViewCacheCursor& cursor, const uint256 &hashBlock) override;
    //! Encode and commit a flush snapshot without holding cs_main (caller finalizes cache after).
    bool BatchWriteFromSnapshot(const CoinsFlushSnapshot& snapshot);
    std::unique_ptr<CCoinsViewCursor> Cursor() const override;

    //! Whether an unsupported database format is used.
    bool NeedsUpgrade();
    size_t EstimateSize() const override;

    //! Dynamically alter the underlying LMDB reader pool / environment tuning.
    void ResizeCache(size_t new_cache_size) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Request mdb_env_sync on the final WriteBatch of the next BatchWrite (reset after use).
    void SetSyncFinalBatch(bool sync) { m_options.sync_final_batch = sync; }

    //! Override parallel prefetch worker count (e.g. segment replay tests).
    void SetCoinPrefetchWorkers(int workers) { m_options.coin_prefetch_workers = workers; }

    const compress::UtxoZstd& GetUtxoZstdDictionary() const { return m_utxo_zstd; }

    //! LMDB reader slot limit configured for this database.
    unsigned int GetMaxReaders() const;

    //! @returns filesystem path to on-disk storage or std::nullopt if in memory.
    std::optional<fs::path> StoragePath() { return m_db->StoragePath(); }
};

struct PendingCoinWrite {
    COutPoint outpoint;
    bool spent{false};
    Coin coin;
    std::vector<uint8_t> encoded;
    //! Non-null when Sync() defers cache finalization until after durable write (legacy in-lock path).
    CoinsCachePair* cache_pair{nullptr};
};

//! Parallel LMDB reads for coin prefetch (read-only; merge via CCoinsViewCache::WarmCache).
bool ParallelPrefetchCoins(CCoinsViewDB& db,
                           const std::vector<COutPoint>& prevouts,
                           std::vector<PrefetchedCoin>& out,
                           int num_workers);

#endif // BITCOIN_TXDB_H
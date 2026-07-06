// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMPRESS_DICT_BOOTSTRAP_H
#define BITCOIN_COMPRESS_DICT_BOOTSTRAP_H

#include <compress/dict_classify.h>
#include <compress/zstd.h>
#include <util/fs.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class ArgsManager;
class CBlock;
class CChainParams;
class Coin;

namespace compress {

enum class BootstrapPhase {
    NONE,
    PASS1_IN_PROGRESS,
    PASS1_COMPLETE,
    PASS2_IN_PROGRESS,
    COMPLETE,
};

const char* BootstrapPhaseName(BootstrapPhase phase);

/** High bit of UTXO type byte marking typed-bucket encoding (0x80|bucket_id). */
static constexpr uint8_t COIN_VALUE_TYPED_BUCKET{0x80};

struct BootstrapMetrics {
    std::array<std::atomic<uint64_t>, NUM_BLOCK_BUCKETS> block_plaintext_bytes{};
    std::array<std::atomic<uint64_t>, NUM_UTXO_BUCKETS> utxo_plaintext_bytes{};
    std::array<std::atomic<uint64_t>, NUM_BLOCK_BUCKETS> block_stored_bytes{};
    std::array<std::atomic<uint64_t>, NUM_UTXO_BUCKETS> utxo_stored_bytes{};
    std::chrono::steady_clock::time_point pass1_start{};
    std::chrono::steady_clock::time_point pass1_end{};
    std::chrono::steady_clock::time_point pass2_start{};
    std::chrono::steady_clock::time_point pass2_end{};
    int64_t pass1_start_wall_unix{0};
};

/** Stratified per-bucket reservoir sample collector (pass 1). */
class DictSampleCollector
{
public:
    static constexpr size_t MAX_SAMPLES_PER_BUCKET{256};
    static constexpr size_t MAX_SAMPLE_BYTES{64 * 1024};

    explicit DictSampleCollector(fs::path samples_dir);

    void CollectBlockSample(BlockBucket bucket, std::span<const uint8_t> payload);
    void CollectUtxoSample(UtxoBucket bucket, std::span<const uint8_t> payload);

    uint64_t BlockSampleBytes(BlockBucket bucket) const;
    uint64_t UtxoSampleBytes(UtxoBucket bucket) const;

    std::vector<std::span<const uint8_t>> LoadBlockSamples(BlockBucket bucket) const;
    std::vector<std::span<const uint8_t>> LoadUtxoSamples(UtxoBucket bucket) const;

    uint64_t BlockSeenCount(BlockBucket bucket) const;
    uint64_t UtxoSeenCount(UtxoBucket bucket) const;
    void RestoreSeenCounts(const std::array<uint64_t, NUM_BLOCK_BUCKETS>& block_seen,
                           const std::array<uint64_t, NUM_UTXO_BUCKETS>& utxo_seen);

private:
    mutable std::mutex m_mutex;
    fs::path m_samples_dir;
    std::array<std::atomic<uint64_t>, NUM_BLOCK_BUCKETS> m_block_sample_bytes{};
    std::array<std::atomic<uint64_t>, NUM_UTXO_BUCKETS> m_utxo_sample_bytes{};
    std::array<uint64_t, NUM_BLOCK_BUCKETS> m_block_seen{};
    std::array<uint64_t, NUM_UTXO_BUCKETS> m_utxo_seen{};
    std::array<std::vector<std::vector<uint8_t>>, NUM_BLOCK_BUCKETS> m_block_samples{};
    std::array<std::vector<std::vector<uint8_t>>, NUM_UTXO_BUCKETS> m_utxo_samples{};

    void ReservoirAppendLocked(std::vector<std::vector<uint8_t>>& samples, uint64_t& seen,
                               std::span<const uint8_t> payload);
    bool WriteSampleFile(const fs::path& path, const std::vector<std::vector<uint8_t>>& samples) const;
    std::vector<std::vector<uint8_t>> ReadSampleFile(const fs::path& path) const;
    void LoadSamplesFromDisk();
    fs::path BlockSamplePath(BlockBucket bucket) const;
    fs::path UtxoSamplePath(UtxoBucket bucket) const;
};

/** Background incremental dictionary trainer (pass 1). */
class DictionaryTrainer
{
public:
    static constexpr auto RETRAIN_INTERVAL{std::chrono::minutes{30}};

    DictionaryTrainer(fs::path dicts_dir, DictSampleCollector& collector, uint32_t inscription_zero_height);
    ~DictionaryTrainer();

    void Start();
    void Stop();
    void NotifySamplesChanged();
    void TrainAllBuckets(bool final_train);

private:
    fs::path m_dicts_dir;
    DictSampleCollector& m_collector;
    uint32_t m_inscription_zero_height;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_samples_changed{false};
    std::array<std::atomic<uint64_t>, NUM_BLOCK_BUCKETS> m_last_block_train_bytes{};
    std::array<std::atomic<uint64_t>, NUM_UTXO_BUCKETS> m_last_utxo_train_bytes{};
    std::chrono::steady_clock::time_point m_last_periodic_train{std::chrono::steady_clock::now()};

    void ThreadMain();
    void TrainBucket(const std::string& kind, uint8_t bucket_id, const std::string& name,
                     std::span<const std::span<const uint8_t>> samples, bool provisional);
    void PromoteProvisionalDicts();
};

/** Typed dictionary bootstrap state machine (mainnet only). */
class DictBootstrapManager
{
public:
    DictBootstrapManager(const fs::path& datadir, const CChainParams& params, const ArgsManager& args);
    ~DictBootstrapManager();

    BootstrapPhase Phase() const { return m_phase; }
    bool IsPass1InProgress() const { return m_phase == BootstrapPhase::PASS1_IN_PROGRESS; }
    bool IsPass2InProgress() const { return m_phase == BootstrapPhase::PASS2_IN_PROGRESS; }
    /** True during pass 2 reindex when blocks must be rewritten via WriteBlock. */
    bool ShouldRewriteBlocksOnReindex() const { return IsPass2InProgress() && ShouldCompressOnWrite(); }
    bool ShouldCompressOnWrite() const;
    bool UseTypedBlockFormat() const;
    bool UseTypedUtxoFormat() const;

    /** Called after chainstate load; starts pass 1 only when IBD is active. */
    void OnChainReady(bool in_ibd);

    void OnBlockWritten(int height, const CBlock& block, std::span<const uint8_t> payload);
    void OnCoinEncoded(const Coin& coin, std::span<const uint8_t> serialized);

    uint8_t BlockFlagsForBucket(BlockBucket bucket) const;
    uint8_t UtxoTypeByteForBucket(UtxoBucket bucket) const;

    void OnIbdComplete();

    /** Called when bootstrap is pass1_complete and node starts with -reindex. */
    void EnterPass2();

    /** Called after block/UTXO reindex completes during pass 2. */
    void OnReindexComplete();

    void OnBlockStored(BlockBucket bucket, size_t plaintext_bytes, size_t stored_bytes);
    void OnCoinStored(UtxoBucket bucket, size_t plaintext_bytes, size_t stored_bytes);

    const DictSet& Dicts() const;
    const DictZstd& BlockDict(BlockBucket bucket) const;
    const DictZstd& UtxoDict(UtxoBucket bucket) const;

    const BootstrapMetrics& Metrics() const { return m_metrics; }

    static bool IsBootstrapEnabledForChain(const CChainParams& params);
    static bool IsValidBootstrapMode(const std::string& mode);

private:
    enum class LoadStateResult {
        MISSING,
        OK,
        CORRUPT,
    };

    fs::path m_datadir;
    const CChainParams& m_params;
    BootstrapPhase m_phase{BootstrapPhase::NONE};
    bool m_enabled{false};
    bool m_awaiting_chain{false};
    BootstrapMetrics m_metrics;
    std::unique_ptr<DictSampleCollector> m_collector;
    std::unique_ptr<DictionaryTrainer> m_trainer;
    std::unique_ptr<DictSet> m_dict_set;
    fs::path m_state_path;
    std::mutex m_complete_mutex;
    std::atomic<bool> m_completing{false};
    std::thread m_complete_thread;
    std::array<uint64_t, NUM_BLOCK_BUCKETS> m_block_seen{};
    std::array<uint64_t, NUM_UTXO_BUCKETS> m_utxo_seen{};

    void JoinCompleteThread();
    LoadStateResult LoadState();
    bool SaveState() const;
    void EnterPass1();
    void CompletePass1();
    void CompletePass2();
    void LoadDictSet();
    bool SaveCompressionReport() const;
    bool SaveBaseline() const;
    void SkipPass1AlreadySynced();
    void ResumePass1();
    BootstrapPhase ParsePhase(const std::string& state) const;
    std::string PhaseToString(BootstrapPhase phase) const;
};

extern std::unique_ptr<DictBootstrapManager> g_dict_bootstrap;

/** @return false when -dictbootstrap is invalid (init should abort). */
bool InitDictBootstrap(const fs::path& datadir, const CChainParams& params, const ArgsManager& args, bool do_reindex);
void ShutdownDictBootstrap();

} // namespace compress

#endif // BITCOIN_COMPRESS_DICT_BOOTSTRAP_H
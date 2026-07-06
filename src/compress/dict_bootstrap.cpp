// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compress/dict_bootstrap.h>

#include <common/args.h>
#include <kernel/chainparams.h>
#include <node/interface_ui.h>
#include <logging.h>
#include <node/blockfile_format.h>
#include <primitives/block.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>

#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/threadnames.h>
#include <util/time.h>
#include <util/translation.h>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <span.h>

namespace compress {

std::unique_ptr<DictBootstrapManager> g_dict_bootstrap;

namespace {

uint32_t ReadLE32(std::span<const uint8_t> bytes)
{
    if (bytes.size() < 4) return 0;
    return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) | (uint32_t{bytes[2]} << 16) | (uint32_t{bytes[3]} << 24);
}

void WriteLE32(std::span<uint8_t> dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t>(value);
    dest[1] = static_cast<uint8_t>(value >> 8);
    dest[2] = static_cast<uint8_t>(value >> 16);
    dest[3] = static_cast<uint8_t>(value >> 24);
}

std::chrono::steady_clock::time_point SteadyTimeFromUs(const int64_t us)
{
    return std::chrono::steady_clock::time_point{std::chrono::microseconds{us}};
}

bool WriteManifestEntry(const fs::path& manifest_path, const std::string& kind, const std::string& name,
                        uint8_t bucket_id, const std::string& filename, const TrainDictionaryResult& result,
                        uint32_t inscription_zero_height)
{
    UniValue root{UniValue::VOBJ};
    UniValue buckets{UniValue::VARR};

    if (fs::exists(manifest_path)) {
        std::ifstream in{manifest_path};
        UniValue existing;
        if (in.is_open() && existing.read(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()})
            && existing.isObject() && existing["buckets"].isArray()) {
            buckets = existing["buckets"];
        }
        if (existing.isObject() && existing["inscription_zero_height"].isNum()) {
            root.pushKV("inscription_zero_height", existing["inscription_zero_height"]);
        }
    }
    if (!root.exists("inscription_zero_height")) {
        root.pushKV("inscription_zero_height", static_cast<int64_t>(inscription_zero_height));
    }

    bool replaced{false};
    UniValue updated_buckets{UniValue::VARR};
    for (size_t i = 0; i < buckets.size(); ++i) {
        const UniValue& existing{buckets[i]};
        if (!replaced && existing.isObject() && existing["name"].isStr() && existing["name"].get_str() == name) {
            UniValue entry{UniValue::VOBJ};
            entry.pushKV("name", name);
            entry.pushKV("kind", kind);
            entry.pushKV("bucket_id", bucket_id);
            entry.pushKV("filename", filename);
            entry.pushKV("chosen_size", static_cast<int64_t>(result.chosen_size));
            entry.pushKV("holdout_ratio", result.holdout_ratio);
            entry.pushKV("sample_bytes", static_cast<int64_t>(result.sample_bytes));
            UniValue tried{UniValue::VARR};
            for (const size_t candidate : result.candidates_tried) {
                tried.push_back(static_cast<int64_t>(candidate));
            }
            entry.pushKV("candidates_tried", tried);
            updated_buckets.push_back(entry);
            replaced = true;
        } else {
            updated_buckets.push_back(existing);
        }
    }
    buckets = updated_buckets;
    if (!replaced) {
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("name", name);
        entry.pushKV("kind", kind);
        entry.pushKV("bucket_id", bucket_id);
        entry.pushKV("filename", filename);
        entry.pushKV("chosen_size", static_cast<int64_t>(result.chosen_size));
        entry.pushKV("holdout_ratio", result.holdout_ratio);
        entry.pushKV("sample_bytes", static_cast<int64_t>(result.sample_bytes));
        UniValue tried{UniValue::VARR};
        for (const size_t candidate : result.candidates_tried) {
            tried.push_back(static_cast<int64_t>(candidate));
        }
        entry.pushKV("candidates_tried", tried);
        buckets.push_back(entry);
    }
    root.pushKV("buckets", buckets);

    const fs::path tmp{fs::u8path(fs::PathToString(manifest_path) + ".tmp")};
    std::ofstream out{tmp};
    if (!out.is_open()) return false;
    out << root.write(4, 1) << std::endl;
    out.close();
    std::error_code ec;
    fs::rename(tmp, manifest_path, ec);
    return !ec;
}

} // namespace

const char* BootstrapPhaseName(BootstrapPhase phase)
{
    switch (phase) {
    case BootstrapPhase::NONE: return "none";
    case BootstrapPhase::PASS1_IN_PROGRESS: return "pass1_in_progress";
    case BootstrapPhase::PASS1_COMPLETE: return "pass1_complete";
    case BootstrapPhase::PASS2_IN_PROGRESS: return "pass2_in_progress";
    case BootstrapPhase::COMPLETE: return "complete";
    }
    return "unknown";
}

DictSampleCollector::DictSampleCollector(fs::path samples_dir)
    : m_samples_dir{std::move(samples_dir)}
{
    fs::create_directories(m_samples_dir);
    LoadSamplesFromDisk();
}

fs::path DictSampleCollector::BlockSamplePath(BlockBucket bucket) const
{
    return m_samples_dir / fs::u8path(std::string{"blk_"} + BlockBucketName(bucket) + ".samples");
}

fs::path DictSampleCollector::UtxoSamplePath(UtxoBucket bucket) const
{
    return m_samples_dir / fs::u8path(std::string{"utxo_"} + UtxoBucketName(bucket) + ".samples");
}

bool DictSampleCollector::WriteSampleFile(const fs::path& path,
                                          const std::vector<std::vector<uint8_t>>& samples) const
{
    const fs::path tmp{fs::u8path(fs::PathToString(path) + ".tmp")};
    FILE* file{fsbridge::fopen(tmp, "wb")};
    if (file == nullptr) return false;
    AutoFile auto_file{file};
    try {
        for (const auto& sample : samples) {
            std::vector<uint8_t> record(4 + sample.size());
            WriteLE32(record, static_cast<uint32_t>(sample.size()));
            std::copy(sample.begin(), sample.end(), record.begin() + 4);
            auto_file.write(MakeByteSpan(record));
        }
        auto_file.fclose();
    } catch (...) {
        auto_file.fclose();
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

void DictSampleCollector::ReservoirAppendLocked(std::vector<std::vector<uint8_t>>& samples, uint64_t& seen,
                                                const std::span<const uint8_t> payload)
{
    if (payload.empty() || payload.size() > MAX_SAMPLE_BYTES) return;

    ++seen;
    if (samples.size() < MAX_SAMPLES_PER_BUCKET) {
        samples.emplace_back(payload.begin(), payload.end());
        return;
    }

    FastRandomContext rng;
    const uint64_t reservoir_idx{rng.randrange(seen)};
    if (reservoir_idx < MAX_SAMPLES_PER_BUCKET) {
        samples[static_cast<size_t>(reservoir_idx)] = std::vector<uint8_t>(payload.begin(), payload.end());
    }
}

std::vector<std::vector<uint8_t>> DictSampleCollector::ReadSampleFile(const fs::path& path) const
{
    std::vector<std::vector<uint8_t>> samples;
    if (!fs::exists(path)) return samples;

    std::vector<uint8_t> file_data;
    FILE* file{fsbridge::fopen(path, "rb")};
    if (file == nullptr) return samples;
    AutoFile auto_file{file};
    try {
        const auto size_u{fs::file_size(path)};
        const int64_t size{size_u > static_cast<uintmax_t>(std::numeric_limits<int64_t>::max())
                               ? std::numeric_limits<int64_t>::max()
                               : static_cast<int64_t>(size_u)};
        if (size <= 0) return samples;
        file_data.resize(static_cast<size_t>(size));
        auto_file.read(MakeWritableByteSpan(file_data));
        auto_file.fclose();
    } catch (...) {
        auto_file.fclose();
        return samples;
    }

    size_t offset{0};
    while (offset + 4 <= file_data.size()) {
        const uint32_t len{ReadLE32(std::span<const uint8_t>{file_data}.subspan(offset, 4))};
        offset += 4;
        if (len == 0 || offset + len > file_data.size()) break;
        samples.emplace_back(file_data.begin() + static_cast<std::ptrdiff_t>(offset),
                             file_data.begin() + static_cast<std::ptrdiff_t>(offset + len));
        offset += len;
    }

    if (samples.size() > MAX_SAMPLES_PER_BUCKET) {
        samples.erase(samples.begin(), samples.end() - static_cast<std::ptrdiff_t>(MAX_SAMPLES_PER_BUCKET));
    }
    return samples;
}

void DictSampleCollector::LoadSamplesFromDisk()
{
    std::lock_guard lock{m_mutex};
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        m_block_samples[i] = ReadSampleFile(BlockSamplePath(static_cast<BlockBucket>(i)));
        m_block_seen[i] = m_block_samples[i].size();
    }
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        m_utxo_samples[i] = ReadSampleFile(UtxoSamplePath(static_cast<UtxoBucket>(i)));
        m_utxo_seen[i] = m_utxo_samples[i].size();
    }
}

uint64_t DictSampleCollector::BlockSeenCount(const BlockBucket bucket) const
{
    std::lock_guard lock{m_mutex};
    return m_block_seen[static_cast<size_t>(bucket)];
}

uint64_t DictSampleCollector::UtxoSeenCount(const UtxoBucket bucket) const
{
    std::lock_guard lock{m_mutex};
    return m_utxo_seen[static_cast<size_t>(bucket)];
}

void DictSampleCollector::RestoreSeenCounts(const std::array<uint64_t, NUM_BLOCK_BUCKETS>& block_seen,
                                            const std::array<uint64_t, NUM_UTXO_BUCKETS>& utxo_seen)
{
    std::lock_guard lock{m_mutex};
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        m_block_seen[i] = std::max(m_block_seen[i], block_seen[i]);
    }
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        m_utxo_seen[i] = std::max(m_utxo_seen[i], utxo_seen[i]);
    }
}

void DictSampleCollector::CollectBlockSample(BlockBucket bucket, std::span<const uint8_t> payload)
{
    std::lock_guard lock{m_mutex};
    const size_t idx{static_cast<size_t>(bucket)};
    m_block_sample_bytes[idx].fetch_add(payload.size(), std::memory_order_relaxed);
    ReservoirAppendLocked(m_block_samples[idx], m_block_seen[idx], payload);
    if (!WriteSampleFile(BlockSamplePath(bucket), m_block_samples[idx])) {
        LogPrintf("Warning: failed to persist dictionary bootstrap block samples to %s\n",
                  fs::PathToString(BlockSamplePath(bucket)));
    }
}

void DictSampleCollector::CollectUtxoSample(UtxoBucket bucket, std::span<const uint8_t> payload)
{
    std::lock_guard lock{m_mutex};
    const size_t idx{static_cast<size_t>(bucket)};
    m_utxo_sample_bytes[idx].fetch_add(payload.size(), std::memory_order_relaxed);
    ReservoirAppendLocked(m_utxo_samples[idx], m_utxo_seen[idx], payload);
    if (!WriteSampleFile(UtxoSamplePath(bucket), m_utxo_samples[idx])) {
        LogPrintf("Warning: failed to persist dictionary bootstrap UTXO samples to %s\n",
                  fs::PathToString(UtxoSamplePath(bucket)));
    }
}

uint64_t DictSampleCollector::BlockSampleBytes(BlockBucket bucket) const
{
    return m_block_sample_bytes[static_cast<size_t>(bucket)].load(std::memory_order_relaxed);
}

uint64_t DictSampleCollector::UtxoSampleBytes(UtxoBucket bucket) const
{
    return m_utxo_sample_bytes[static_cast<size_t>(bucket)].load(std::memory_order_relaxed);
}

std::vector<std::span<const uint8_t>> DictSampleCollector::LoadBlockSamples(BlockBucket bucket) const
{
    std::lock_guard lock{m_mutex};
    static thread_local std::vector<std::vector<uint8_t>> storage;
    storage = m_block_samples[static_cast<size_t>(bucket)];
    std::vector<std::span<const uint8_t>> spans;
    spans.reserve(storage.size());
    for (const auto& sample : storage) {
        spans.emplace_back(sample);
    }
    return spans;
}

std::vector<std::span<const uint8_t>> DictSampleCollector::LoadUtxoSamples(UtxoBucket bucket) const
{
    std::lock_guard lock{m_mutex};
    static thread_local std::vector<std::vector<uint8_t>> storage;
    storage = m_utxo_samples[static_cast<size_t>(bucket)];
    std::vector<std::span<const uint8_t>> spans;
    spans.reserve(storage.size());
    for (const auto& sample : storage) {
        spans.emplace_back(sample);
    }
    return spans;
}

DictionaryTrainer::DictionaryTrainer(fs::path dicts_dir, DictSampleCollector& collector,
                                     const uint32_t inscription_zero_height)
    : m_dicts_dir{std::move(dicts_dir)}, m_collector{collector}, m_inscription_zero_height{inscription_zero_height}
{
    fs::create_directories(m_dicts_dir);
}

DictionaryTrainer::~DictionaryTrainer()
{
    Stop();
}

void DictionaryTrainer::Start()
{
    if (m_thread.joinable()) return;
    m_stop.store(false, std::memory_order_relaxed);
    m_thread = std::thread([this] { ThreadMain(); });
}

void DictionaryTrainer::Stop()
{
    m_stop.store(true, std::memory_order_relaxed);
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void DictionaryTrainer::NotifySamplesChanged()
{
    m_samples_changed.store(true, std::memory_order_relaxed);
    m_cv.notify_one();
}

void DictionaryTrainer::TrainBucket(const std::string& kind, const uint8_t bucket_id, const std::string& name,
                                      std::span<const std::span<const uint8_t>> samples, const bool provisional)
{
    if (samples.size() < 2) return;
    const auto result{TrainDictionary(samples)};
    if (!result) return;

    const std::string suffix{provisional ? ".dict.provisional" : ".dict"};
    const fs::path dict_path{m_dicts_dir / fs::u8path(name + suffix)};
    const fs::path tmp{fs::u8path(fs::PathToString(dict_path) + ".tmp")};
    {
        FILE* file{fsbridge::fopen(tmp, "wb")};
        if (file == nullptr) return;
        AutoFile auto_file{file};
        try {
            auto_file.write(MakeByteSpan(result->dictionary));
            auto_file.fclose();
        } catch (...) {
            auto_file.fclose();
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, dict_path, ec);
    if (ec) return;

    const fs::path manifest_path{m_dicts_dir.parent_path() / "dict_manifest.json"};
    WriteManifestEntry(manifest_path, kind, name, bucket_id, fs::PathToString(dict_path.filename()), *result,
                       m_inscription_zero_height);
    LogPrintf("Dictionary bootstrap trained %s bucket %s (%zu bytes, holdout ratio %.3f, provisional=%d)\n",
              kind, name, result->chosen_size, result->holdout_ratio, provisional);
}

void DictionaryTrainer::PromoteProvisionalDicts()
{
    for (const auto& entry : fs::directory_iterator(m_dicts_dir)) {
        const std::string filename{fs::PathToString(entry.path().filename())};
        if (!entry.is_regular_file() || !filename.ends_with(".dict.provisional")) continue;
        const fs::path final_path{entry.path().parent_path() / filename.substr(0, filename.size() - strlen(".provisional"))};
        std::error_code ec;
        fs::rename(entry.path(), final_path, ec);
    }
}

void DictionaryTrainer::TrainAllBuckets(const bool final_train)
{
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        const auto bucket{static_cast<BlockBucket>(i)};
        const auto samples{m_collector.LoadBlockSamples(bucket)};
        TrainBucket("block", static_cast<uint8_t>(i), BlockBucketName(bucket), samples, !final_train);
        m_last_block_train_bytes[i].store(m_collector.BlockSampleBytes(bucket), std::memory_order_relaxed);
    }
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        const auto bucket{static_cast<UtxoBucket>(i)};
        const auto samples{m_collector.LoadUtxoSamples(bucket)};
        TrainBucket("utxo", static_cast<uint8_t>(i), UtxoBucketName(bucket), samples, !final_train);
        m_last_utxo_train_bytes[i].store(m_collector.UtxoSampleBytes(bucket), std::memory_order_relaxed);
    }
    if (final_train) {
        PromoteProvisionalDicts();
    }
}

void DictionaryTrainer::ThreadMain()
{
    util::ThreadRename("dicttrainer");
    while (!m_stop.load(std::memory_order_relaxed)) {
        std::unique_lock lock{m_mutex};
        m_cv.wait_for(lock, std::chrono::seconds{30}, [this] {
            return m_stop.load(std::memory_order_relaxed) || m_samples_changed.load(std::memory_order_relaxed);
        });
        if (m_stop.load(std::memory_order_relaxed)) break;
        m_samples_changed.store(false, std::memory_order_relaxed);

        bool should_train{false};
        const auto now{std::chrono::steady_clock::now()};
        if (now - m_last_periodic_train >= RETRAIN_INTERVAL) {
            should_train = true;
            m_last_periodic_train = now;
        }

        for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
            const uint64_t current{m_collector.BlockSampleBytes(static_cast<BlockBucket>(i))};
            const uint64_t last{m_last_block_train_bytes[i].load(std::memory_order_relaxed)};
            if (last > 0 && current >= last * 2) {
                should_train = true;
            }
        }
        for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
            const uint64_t current{m_collector.UtxoSampleBytes(static_cast<UtxoBucket>(i))};
            const uint64_t last{m_last_utxo_train_bytes[i].load(std::memory_order_relaxed)};
            if (last > 0 && current >= last * 2) {
                should_train = true;
            }
        }

        lock.unlock();
        if (should_train) {
            TrainAllBuckets(/*final_train=*/false);
        }
    }
}

bool DictBootstrapManager::IsBootstrapEnabledForChain(const CChainParams& params)
{
    return params.GetChainType() == ChainType::MAIN && InscriptionZeroHeight(params).has_value();
}

bool DictBootstrapManager::IsValidBootstrapMode(const std::string& mode)
{
    return mode == "auto" || mode == "off";
}

DictBootstrapManager::DictBootstrapManager(const fs::path& datadir, const CChainParams& params, const ArgsManager& args)
    : m_datadir{datadir}, m_params{params}, m_state_path{datadir / "swords" / "bootstrap_state.json"}
{
    if (!IsBootstrapEnabledForChain(params)) {
        return;
    }

    const std::string mode{args.GetArg("-dictbootstrap", "auto")};
    if (!IsValidBootstrapMode(mode)) {
        return;
    }
    if (mode == "off") {
        if (fs::exists(m_state_path)) {
            LogPrintf("Dictionary bootstrap disabled (-dictbootstrap=off); ignoring existing bootstrap state at %s\n",
                      fs::PathToString(m_state_path));
        }
        return;
    }

    m_enabled = true;
    const LoadStateResult load_result{LoadState()};
    if (load_result == LoadStateResult::CORRUPT) {
        m_enabled = false;
        InitWarning(Untranslated(strprintf(
            "Dictionary bootstrap state file is corrupt (%s). "
            "Delete the file or run with -dictbootstrap=off to recover.",
            fs::PathToString(m_state_path))));
        return;
    }
    if (load_result == LoadStateResult::MISSING) {
        m_awaiting_chain = true;
        return;
    }

    switch (m_phase) {
    case BootstrapPhase::PASS1_IN_PROGRESS:
        ResumePass1();
        break;
    case BootstrapPhase::PASS1_COMPLETE:
        LogPrintf("Dictionary bootstrap pass 1 complete; restart with -reindex for compression pass 2\n");
        break;
    case BootstrapPhase::PASS2_IN_PROGRESS:
        LoadDictSet();
        LogPrintf("Dictionary bootstrap pass 2 in progress (compression reindex)\n");
        break;
    case BootstrapPhase::COMPLETE:
        LoadDictSet();
        LogPrintf("Dictionary bootstrap complete; typed dictionaries active\n");
        break;
    default:
        break;
    }
}

void DictBootstrapManager::JoinCompleteThread()
{
    if (m_complete_thread.joinable()) {
        m_complete_thread.join();
    }
}

DictBootstrapManager::~DictBootstrapManager()
{
    JoinCompleteThread();
    if (IsPass1InProgress()) {
        SaveState();
    }
    if (m_trainer) {
        m_trainer->Stop();
    }
}

bool DictBootstrapManager::ShouldCompressOnWrite() const
{
    switch (m_phase) {
    case BootstrapPhase::PASS1_IN_PROGRESS:
    case BootstrapPhase::PASS1_COMPLETE:
        return false;
    case BootstrapPhase::PASS2_IN_PROGRESS:
    case BootstrapPhase::COMPLETE:
        return true;
    case BootstrapPhase::NONE:
        return true;
    }
    return true;
}

bool DictBootstrapManager::UseTypedBlockFormat() const
{
    return m_phase == BootstrapPhase::PASS1_IN_PROGRESS || m_phase == BootstrapPhase::PASS2_IN_PROGRESS
        || m_phase == BootstrapPhase::COMPLETE;
}

bool DictBootstrapManager::UseTypedUtxoFormat() const
{
    return m_phase == BootstrapPhase::PASS1_IN_PROGRESS || m_phase == BootstrapPhase::PASS2_IN_PROGRESS
        || m_phase == BootstrapPhase::COMPLETE;
}

void DictBootstrapManager::OnChainReady(const bool in_ibd)
{
    if (!m_enabled) return;

    if (m_awaiting_chain) {
        m_awaiting_chain = false;
        if (in_ibd) {
            EnterPass1();
        } else {
            SkipPass1AlreadySynced();
        }
        return;
    }

    if (IsPass1InProgress() && !in_ibd) {
        OnIbdComplete();
    }
}

uint8_t DictBootstrapManager::BlockFlagsForBucket(const BlockBucket bucket) const
{
    return node::BlockBucketFlags(static_cast<uint8_t>(bucket));
}

uint8_t DictBootstrapManager::UtxoTypeByteForBucket(const UtxoBucket bucket) const
{
    return static_cast<uint8_t>(COIN_VALUE_TYPED_BUCKET | static_cast<uint8_t>(bucket));
}

void DictBootstrapManager::OnBlockWritten(const int height, const CBlock& block, const std::span<const uint8_t> payload)
{
    if (!IsPass1InProgress() || !m_collector) return;
    const BlockBucket bucket{ClassifyBlock(height, block, m_params)};
    m_collector->CollectBlockSample(bucket, payload);
    m_metrics.block_plaintext_bytes[static_cast<size_t>(bucket)].fetch_add(payload.size(), std::memory_order_relaxed);
    if (m_trainer) {
        m_trainer->NotifySamplesChanged();
    }
}

void DictBootstrapManager::OnCoinEncoded(const Coin& coin, const std::span<const uint8_t> serialized)
{
    if (!IsPass1InProgress() || !m_collector) return;
    const UtxoBucket bucket{ClassifyCoin(coin, m_params)};
    m_collector->CollectUtxoSample(bucket, serialized);
    m_metrics.utxo_plaintext_bytes[static_cast<size_t>(bucket)].fetch_add(serialized.size(), std::memory_order_relaxed);
    if (m_trainer) {
        m_trainer->NotifySamplesChanged();
    }
}

BootstrapPhase DictBootstrapManager::ParsePhase(const std::string& state) const
{
    if (state == "pass1_in_progress") return BootstrapPhase::PASS1_IN_PROGRESS;
    if (state == "pass1_complete") return BootstrapPhase::PASS1_COMPLETE;
    if (state == "pass2_in_progress") return BootstrapPhase::PASS2_IN_PROGRESS;
    if (state == "complete") return BootstrapPhase::COMPLETE;
    return BootstrapPhase::NONE;
}

std::string DictBootstrapManager::PhaseToString(const BootstrapPhase phase) const
{
    return BootstrapPhaseName(phase);
}

DictBootstrapManager::LoadStateResult DictBootstrapManager::LoadState()
{
    if (!fs::exists(m_state_path)) return LoadStateResult::MISSING;
    std::ifstream file{m_state_path};
    if (!file.is_open()) return LoadStateResult::CORRUPT;
    UniValue json;
    if (!json.read(std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}) || !json.isObject()) {
        return LoadStateResult::CORRUPT;
    }
    if (!json["state"].isStr()) return LoadStateResult::CORRUPT;
    m_phase = ParsePhase(json["state"].get_str());
    if (m_phase == BootstrapPhase::NONE) return LoadStateResult::CORRUPT;

    if (json["pass1_start_steady_us"].isNum()) {
        m_metrics.pass1_start = SteadyTimeFromUs(json["pass1_start_steady_us"].getInt<int64_t>());
    }
    if (json["pass1_end_steady_us"].isNum()) {
        m_metrics.pass1_end = SteadyTimeFromUs(json["pass1_end_steady_us"].getInt<int64_t>());
    }
    if (json["pass1_start_wall_unix"].isNum()) {
        m_metrics.pass1_start_wall_unix = json["pass1_start_wall_unix"].getInt<int64_t>();
    }
    if (json["block_plaintext_bytes"].isObject()) {
        const UniValue& block_metrics{json["block_plaintext_bytes"]};
        for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
            const std::string name{BlockBucketName(static_cast<BlockBucket>(i))};
            if (block_metrics[name].isNum()) {
                m_metrics.block_plaintext_bytes[i].store(block_metrics[name].getInt<uint64_t>());
            }
        }
    }
    if (json["utxo_plaintext_bytes"].isObject()) {
        const UniValue& utxo_metrics{json["utxo_plaintext_bytes"]};
        for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
            const std::string name{UtxoBucketName(static_cast<UtxoBucket>(i))};
            if (utxo_metrics[name].isNum()) {
                m_metrics.utxo_plaintext_bytes[i].store(utxo_metrics[name].getInt<uint64_t>());
            }
        }
    }
    if (json["block_stored_bytes"].isObject()) {
        const UniValue& block_metrics{json["block_stored_bytes"]};
        for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
            const std::string name{BlockBucketName(static_cast<BlockBucket>(i))};
            if (block_metrics[name].isNum()) {
                m_metrics.block_stored_bytes[i].store(block_metrics[name].getInt<uint64_t>());
            }
        }
    }
    if (json["utxo_stored_bytes"].isObject()) {
        const UniValue& utxo_metrics{json["utxo_stored_bytes"]};
        for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
            const std::string name{UtxoBucketName(static_cast<UtxoBucket>(i))};
            if (utxo_metrics[name].isNum()) {
                m_metrics.utxo_stored_bytes[i].store(utxo_metrics[name].getInt<uint64_t>());
            }
        }
    }
    if (json["pass2_start_steady_us"].isNum()) {
        m_metrics.pass2_start = SteadyTimeFromUs(json["pass2_start_steady_us"].getInt<int64_t>());
    }
    if (json["pass2_end_steady_us"].isNum()) {
        m_metrics.pass2_end = SteadyTimeFromUs(json["pass2_end_steady_us"].getInt<int64_t>());
    }
    if (json["block_samples_seen"].isObject()) {
        const UniValue& seen{json["block_samples_seen"]};
        for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
            const std::string name{BlockBucketName(static_cast<BlockBucket>(i))};
            if (seen[name].isNum()) {
                m_block_seen[i] = seen[name].getInt<uint64_t>();
            }
        }
    }
    if (json["utxo_samples_seen"].isObject()) {
        const UniValue& seen{json["utxo_samples_seen"]};
        for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
            const std::string name{UtxoBucketName(static_cast<UtxoBucket>(i))};
            if (seen[name].isNum()) {
                m_utxo_seen[i] = seen[name].getInt<uint64_t>();
            }
        }
    }
    return LoadStateResult::OK;
}

bool DictBootstrapManager::SaveState() const
{
    fs::create_directories(m_state_path.parent_path());
    UniValue json{UniValue::VOBJ};
    json.pushKV("state", PhaseToString(m_phase));
    if (m_metrics.pass1_start != std::chrono::steady_clock::time_point{}) {
        json.pushKV("pass1_start_steady_us",
                    static_cast<int64_t>(Ticks<std::chrono::microseconds>(m_metrics.pass1_start.time_since_epoch())));
    }
    if (m_metrics.pass1_end != std::chrono::steady_clock::time_point{}) {
        json.pushKV("pass1_end_steady_us",
                    static_cast<int64_t>(Ticks<std::chrono::microseconds>(m_metrics.pass1_end.time_since_epoch())));
    }
    if (m_metrics.pass1_start_wall_unix != 0) {
        json.pushKV("pass1_start_wall_unix", m_metrics.pass1_start_wall_unix);
    }
    UniValue block_metrics{UniValue::VOBJ};
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        block_metrics.pushKV(BlockBucketName(static_cast<BlockBucket>(i)),
                             static_cast<int64_t>(m_metrics.block_plaintext_bytes[i].load()));
    }
    json.pushKV("block_plaintext_bytes", block_metrics);
    UniValue utxo_metrics{UniValue::VOBJ};
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        utxo_metrics.pushKV(UtxoBucketName(static_cast<UtxoBucket>(i)),
                             static_cast<int64_t>(m_metrics.utxo_plaintext_bytes[i].load()));
    }
    json.pushKV("utxo_plaintext_bytes", utxo_metrics);
    UniValue block_stored{UniValue::VOBJ};
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        block_stored.pushKV(BlockBucketName(static_cast<BlockBucket>(i)),
                            static_cast<int64_t>(m_metrics.block_stored_bytes[i].load()));
    }
    json.pushKV("block_stored_bytes", block_stored);
    UniValue utxo_stored{UniValue::VOBJ};
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        utxo_stored.pushKV(UtxoBucketName(static_cast<UtxoBucket>(i)),
                            static_cast<int64_t>(m_metrics.utxo_stored_bytes[i].load()));
    }
    json.pushKV("utxo_stored_bytes", utxo_stored);
    if (m_metrics.pass2_start != std::chrono::steady_clock::time_point{}) {
        json.pushKV("pass2_start_steady_us",
                    static_cast<int64_t>(Ticks<std::chrono::microseconds>(m_metrics.pass2_start.time_since_epoch())));
    }
    if (m_metrics.pass2_end != std::chrono::steady_clock::time_point{}) {
        json.pushKV("pass2_end_steady_us",
                    static_cast<int64_t>(Ticks<std::chrono::microseconds>(m_metrics.pass2_end.time_since_epoch())));
    }
    if (m_collector) {
        UniValue block_seen{UniValue::VOBJ};
        for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
            block_seen.pushKV(BlockBucketName(static_cast<BlockBucket>(i)),
                              static_cast<int64_t>(m_collector->BlockSeenCount(static_cast<BlockBucket>(i))));
        }
        json.pushKV("block_samples_seen", block_seen);
        UniValue utxo_seen{UniValue::VOBJ};
        for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
            utxo_seen.pushKV(UtxoBucketName(static_cast<UtxoBucket>(i)),
                             static_cast<int64_t>(m_collector->UtxoSeenCount(static_cast<UtxoBucket>(i))));
        }
        json.pushKV("utxo_samples_seen", utxo_seen);
    } else {
        UniValue block_seen{UniValue::VOBJ};
        for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
            block_seen.pushKV(BlockBucketName(static_cast<BlockBucket>(i)), static_cast<int64_t>(m_block_seen[i]));
        }
        json.pushKV("block_samples_seen", block_seen);
        UniValue utxo_seen{UniValue::VOBJ};
        for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
            utxo_seen.pushKV(UtxoBucketName(static_cast<UtxoBucket>(i)), static_cast<int64_t>(m_utxo_seen[i]));
        }
        json.pushKV("utxo_samples_seen", utxo_seen);
    }

    const fs::path tmp{fs::u8path(fs::PathToString(m_state_path) + ".tmp")};
    std::ofstream out{tmp};
    if (!out.is_open()) return false;
    out << json.write(4, 1) << std::endl;
    out.close();
    std::error_code ec;
    fs::rename(tmp, m_state_path, ec);
    return !ec;
}

void DictBootstrapManager::ResumePass1()
{
    const fs::path samples_dir{m_datadir / "swords" / "samples"};
    const fs::path dicts_dir{m_datadir / "swords" / "dicts"};
    const auto ord_height{InscriptionZeroHeight(m_params)};
    m_collector = std::make_unique<DictSampleCollector>(samples_dir);
    m_collector->RestoreSeenCounts(m_block_seen, m_utxo_seen);
    m_trainer = std::make_unique<DictionaryTrainer>(dicts_dir, *m_collector, static_cast<uint32_t>(*ord_height));
    m_trainer->Start();
    if (m_metrics.pass1_start == std::chrono::steady_clock::time_point{}) {
        m_metrics.pass1_start = std::chrono::steady_clock::now();
    }
    if (m_metrics.pass1_start_wall_unix == 0) {
        m_metrics.pass1_start_wall_unix = count_seconds(GetTime<std::chrono::seconds>());
    }
    LogPrintf("Dictionary bootstrap pass 1 resumed (typed sampling, no compression on write)\n");
}

void DictBootstrapManager::EnterPass1()
{
    m_phase = BootstrapPhase::PASS1_IN_PROGRESS;
    m_metrics.pass1_start = std::chrono::steady_clock::now();
    m_metrics.pass1_start_wall_unix = count_seconds(GetTime<std::chrono::seconds>());
    const fs::path samples_dir{m_datadir / "swords" / "samples"};
    const fs::path dicts_dir{m_datadir / "swords" / "dicts"};
    const auto ord_height{InscriptionZeroHeight(m_params)};
    m_collector = std::make_unique<DictSampleCollector>(samples_dir);
    m_trainer = std::make_unique<DictionaryTrainer>(dicts_dir, *m_collector, static_cast<uint32_t>(*ord_height));
    if (!SaveState()) {
        m_phase = BootstrapPhase::NONE;
        m_collector.reset();
        m_trainer.reset();
        InitWarning(Untranslated(strprintf(
            "Failed to persist dictionary bootstrap state to %s; pass 1 not started.",
            fs::PathToString(m_state_path))));
        return;
    }
    m_trainer->Start();
    LogPrintf("Dictionary bootstrap pass 1 started: sampling blocks/UTXOs without compression\n");
}

void DictBootstrapManager::SkipPass1AlreadySynced()
{
    LogPrintf("Dictionary bootstrap pass 1 skipped (chain already synced). "
              "Run a fresh initial sync or -reindex on a new datadir to collect training samples.\n");
}

bool DictBootstrapManager::SaveBaseline() const
{
    const fs::path baseline_path{m_datadir / "swords" / "bootstrap_baseline.json"};
    fs::create_directories(baseline_path.parent_path());

    const int64_t pass1_wall_seconds{
        Ticks<std::chrono::seconds>(m_metrics.pass1_end - m_metrics.pass1_start)};
    const int64_t pass1_end_unix{count_seconds(GetTime<std::chrono::seconds>())};
    const int64_t pass1_start_unix{m_metrics.pass1_start_wall_unix != 0
        ? m_metrics.pass1_start_wall_unix
        : pass1_end_unix - pass1_wall_seconds};

    UniValue json{UniValue::VOBJ};
    json.pushKV("state", "pass1_complete");
    json.pushKV("next_step", "restart with -reindex for compression pass 2");
    json.pushKV("pass1_start", FormatISO8601DateTime(pass1_start_unix));
    json.pushKV("pass1_end", FormatISO8601DateTime(pass1_end_unix));
    json.pushKV("pass1_wall_seconds", pass1_wall_seconds);

    UniValue block_metrics{UniValue::VOBJ};
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        block_metrics.pushKV(BlockBucketName(static_cast<BlockBucket>(i)),
                             static_cast<int64_t>(m_metrics.block_plaintext_bytes[i].load()));
    }
    json.pushKV("block_plaintext_bytes", block_metrics);

    UniValue utxo_metrics{UniValue::VOBJ};
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        utxo_metrics.pushKV(UtxoBucketName(static_cast<UtxoBucket>(i)),
                             static_cast<int64_t>(m_metrics.utxo_plaintext_bytes[i].load()));
    }
    json.pushKV("utxo_plaintext_bytes", utxo_metrics);

    UniValue trained_dicts{UniValue::VARR};
    const fs::path manifest_path{m_datadir / "swords" / "dict_manifest.json"};
    if (fs::exists(manifest_path)) {
        std::ifstream in{manifest_path};
        UniValue manifest;
        if (in.is_open()
            && manifest.read(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()})
            && manifest.isObject() && manifest["buckets"].isArray()) {
            for (size_t i = 0; i < manifest["buckets"].size(); ++i) {
                const UniValue& entry{manifest["buckets"][i]};
                if (!entry.isObject() || !entry["chosen_size"].isNum()) continue;
                UniValue trained{UniValue::VOBJ};
                if (entry["name"].isStr()) {
                    trained.pushKV("name", entry["name"].get_str());
                }
                trained.pushKV("chosen_size", entry["chosen_size"].getInt<int64_t>());
                if (entry["holdout_ratio"].isNum()) {
                    trained.pushKV("holdout_ratio", entry["holdout_ratio"].get_real());
                }
                if (entry["sample_bytes"].isNum()) {
                    trained.pushKV("sample_bytes", entry["sample_bytes"].getInt<int64_t>());
                }
                trained_dicts.push_back(trained);
            }
        }
    }
    json.pushKV("trained_dicts", trained_dicts);

    const fs::path tmp{fs::u8path(fs::PathToString(baseline_path) + ".tmp")};
    std::ofstream out{tmp};
    if (!out.is_open()) return false;
    out << json.write(4, 1) << std::endl;
    out.close();
    std::error_code ec;
    fs::rename(tmp, baseline_path, ec);
    if (ec) return false;
    LogPrintf("Dictionary bootstrap baseline written to %s\n", fs::PathToString(baseline_path));
    return true;
}

void DictBootstrapManager::CompletePass1()
{
    std::lock_guard lock{m_complete_mutex};
    if (!m_trainer || m_phase != BootstrapPhase::PASS1_IN_PROGRESS) return;
    m_trainer->Stop();
    m_trainer->TrainAllBuckets(/*final_train=*/true);
    m_metrics.pass1_end = std::chrono::steady_clock::now();
    m_phase = BootstrapPhase::PASS1_COMPLETE;
    if (!SaveState()) {
        InitWarning(Untranslated(strprintf(
            "Dictionary bootstrap pass 1 finished but failed to persist state to %s; baseline not written.",
            fs::PathToString(m_state_path))));
    } else if (!SaveBaseline()) {
        InitWarning(Untranslated(strprintf(
            "Dictionary bootstrap pass 1 finished but failed to persist baseline to %s.",
            fs::PathToString(m_datadir / "swords" / "bootstrap_baseline.json"))));
    }

    const auto pass1_seconds{Ticks<std::chrono::seconds>(m_metrics.pass1_end - m_metrics.pass1_start)};
    LogPrintf("=== Dictionary bootstrap pass 1 complete (IBD wall time: %d seconds) ===\n", pass1_seconds);
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        LogPrintf("  block %s plaintext_bytes=%llu\n",
                  BlockBucketName(static_cast<BlockBucket>(i)),
                  m_metrics.block_plaintext_bytes[i].load());
    }
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        LogPrintf("  utxo %s plaintext_bytes=%llu\n",
                  UtxoBucketName(static_cast<UtxoBucket>(i)),
                  m_metrics.utxo_plaintext_bytes[i].load());
    }
    LogPrintf("Dictionary bootstrap pass 1 complete; restart with -reindex for compression pass 2\n");
    m_completing.store(false, std::memory_order_relaxed);
}

void DictBootstrapManager::OnIbdComplete()
{
    if (!IsPass1InProgress()) return;
    if (m_completing.exchange(true)) return;
    JoinCompleteThread();
    m_complete_thread = std::thread([this] {
        CompletePass1();
    });
}

void DictBootstrapManager::LoadDictSet()
{
    if (m_dict_set) return;
    m_dict_set = std::make_unique<DictSet>();
    if (!m_dict_set->Load(m_datadir)) {
        LogWarning("Failed to load typed dictionary set from %s; per-bucket compression may be skipped\n",
                   fs::PathToString(m_datadir / "swords" / "dicts"));
    } else {
        size_t loaded_block{0};
        size_t loaded_utxo{0};
        for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
            if (m_dict_set->BlockDict(static_cast<BlockBucket>(i))) ++loaded_block;
        }
        for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
            if (m_dict_set->UtxoDict(static_cast<UtxoBucket>(i))) ++loaded_utxo;
        }
        LogPrintf("Loaded typed dictionary set (%zu manifest entries, %zu block + %zu utxo dicts) from %s\n",
                  m_dict_set->ManifestEntries().size(), loaded_block, loaded_utxo,
                  fs::PathToString(DictSet::ResolveDictsDir(m_datadir)));
        if (m_phase == BootstrapPhase::PASS2_IN_PROGRESS && loaded_block == 0 && loaded_utxo == 0) {
            InitWarning(Untranslated(strprintf(
                "Dictionary bootstrap pass 2: no typed dictionaries loaded from %s; "
                "compression will be skipped until dicts are available.",
                fs::PathToString(DictSet::ResolveDictsDir(m_datadir)))));
        }
    }
}

const DictSet& DictBootstrapManager::Dicts() const
{
    static const DictSet empty;
    return m_dict_set ? *m_dict_set : empty;
}

const DictZstd& DictBootstrapManager::BlockDict(const BlockBucket bucket) const
{
    return Dicts().BlockDict(bucket);
}

const DictZstd& DictBootstrapManager::UtxoDict(const UtxoBucket bucket) const
{
    return Dicts().UtxoDict(bucket);
}

void DictBootstrapManager::EnterPass2()
{
    if (m_phase != BootstrapPhase::PASS1_COMPLETE) return;
    m_phase = BootstrapPhase::PASS2_IN_PROGRESS;
    m_metrics.pass2_start = std::chrono::steady_clock::now();
    LoadDictSet();
    if (!SaveState()) {
        InitWarning(Untranslated(strprintf(
            "Failed to persist dictionary bootstrap pass 2 state to %s.",
            fs::PathToString(m_state_path))));
    }
    LogPrintf("Dictionary bootstrap pass 2 started: compression reindex with typed dictionaries\n");
}

void DictBootstrapManager::OnBlockStored(const BlockBucket bucket, const size_t /*plaintext_bytes*/, const size_t stored_bytes)
{
    if (m_phase != BootstrapPhase::PASS2_IN_PROGRESS) return;
    m_metrics.block_stored_bytes[static_cast<size_t>(bucket)].fetch_add(stored_bytes, std::memory_order_relaxed);
}

void DictBootstrapManager::OnCoinStored(const UtxoBucket bucket, const size_t /*plaintext_bytes*/, const size_t stored_bytes)
{
    if (m_phase != BootstrapPhase::PASS2_IN_PROGRESS) return;
    m_metrics.utxo_stored_bytes[static_cast<size_t>(bucket)].fetch_add(stored_bytes, std::memory_order_relaxed);
}

namespace {

std::string FormatKiB(const size_t bytes)
{
    if (bytes == 0) return "0KiB";
    const size_t kib{(bytes + 1023) / 1024};
    return strprintf("%zuKiB", kib);
}

std::string FormatHumanBytes(const uint64_t bytes)
{
    if (bytes >= 1024ULL * 1024 * 1024) {
        return strprintf("%.2fGiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    if (bytes >= 1024ULL * 1024) {
        return strprintf("%.2fMiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    if (bytes >= 1024) {
        return strprintf("%lluKiB", bytes / 1024);
    }
    return strprintf("%lluB", bytes);
}

int SavingsPercent(const uint64_t plaintext, const uint64_t stored)
{
    if (plaintext == 0) return 0;
    const double saved{1.0 - static_cast<double>(stored) / static_cast<double>(plaintext)};
    return static_cast<int>(saved * 100.0 + 0.5);
}

static constexpr size_t TRAIN_MAX_CAPACITY_KIB{4096};
static constexpr size_t TRAIN_MAX_CAPACITY_BYTES{TRAIN_MAX_CAPACITY_KIB * 1024};
static constexpr double NEAR_MAX_CAPACITY_FRACTION{0.90};
static constexpr int LOW_SAVINGS_PERCENT_THRESHOLD{50};
static constexpr double HIGH_HOLDOUT_RATIO_THRESHOLD{2.0};

struct BucketCompressionInputs {
    std::string name;
    uint64_t plaintext{0};
    uint64_t stored{0};
    std::optional<size_t> dict_size;
    std::optional<double> holdout_ratio;
};

bool ShouldRecommendMoreSamples(const BucketCompressionInputs& bucket)
{
    if (bucket.plaintext == 0 || !bucket.dict_size || !bucket.holdout_ratio) return false;
    if (*bucket.holdout_ratio < HIGH_HOLDOUT_RATIO_THRESHOLD) return false;
    if (*bucket.dict_size < static_cast<size_t>(NEAR_MAX_CAPACITY_FRACTION * static_cast<double>(TRAIN_MAX_CAPACITY_BYTES))) {
        return false;
    }
    return SavingsPercent(bucket.plaintext, bucket.stored) < LOW_SAVINGS_PERCENT_THRESHOLD;
}

std::string FormatCompressionRecommendation(const BucketCompressionInputs& bucket)
{
    const int savings{SavingsPercent(bucket.plaintext, bucket.stored)};
    const int pct_of_max{static_cast<int>(
        *bucket.dict_size * 100 / TRAIN_MAX_CAPACITY_BYTES)};
    return strprintf(
        "%s: holdout=%.2f at %s (%d%% of %zuKiB max); pass 2 savings=%d%% < %d%% — rerun pass 1 IBD for more samples",
        bucket.name,
        *bucket.holdout_ratio,
        FormatKiB(*bucket.dict_size).c_str(),
        pct_of_max,
        TRAIN_MAX_CAPACITY_KIB,
        savings,
        LOW_SAVINGS_PERCENT_THRESHOLD);
}

std::optional<size_t> ManifestDictSize(const DictSet& dicts, const std::string& name)
{
    for (const DictManifestEntry& entry : dicts.ManifestEntries()) {
        if (entry.name == name && entry.chosen_size) {
            return *entry.chosen_size;
        }
    }
    return std::nullopt;
}

std::optional<double> ManifestHoldoutRatio(const DictSet& dicts, const std::string& name)
{
    for (const DictManifestEntry& entry : dicts.ManifestEntries()) {
        if (entry.name == name && entry.holdout_ratio) {
            return *entry.holdout_ratio;
        }
    }
    return std::nullopt;
}

std::vector<std::string> CollectCompressionRecommendations(const DictSet& dicts,
                                                           const std::array<uint64_t, NUM_BLOCK_BUCKETS>& baseline_block_plaintext,
                                                           const std::array<uint64_t, NUM_UTXO_BUCKETS>& baseline_utxo_plaintext,
                                                           const BootstrapMetrics& metrics)
{
    std::vector<std::string> recommendations;
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        const auto bucket{static_cast<BlockBucket>(i)};
        const std::string name{BlockBucketName(bucket)};
        const BucketCompressionInputs inputs{
            name,
            baseline_block_plaintext[i],
            metrics.block_stored_bytes[i].load(),
            ManifestDictSize(dicts, name),
            ManifestHoldoutRatio(dicts, name),
        };
        if (inputs.plaintext == 0 && inputs.stored == 0) continue;
        if (ShouldRecommendMoreSamples(inputs)) {
            recommendations.push_back(FormatCompressionRecommendation(inputs));
        }
    }
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        const auto bucket{static_cast<UtxoBucket>(i)};
        const std::string name{UtxoBucketName(bucket)};
        const BucketCompressionInputs inputs{
            name,
            baseline_utxo_plaintext[i],
            metrics.utxo_stored_bytes[i].load(),
            ManifestDictSize(dicts, name),
            ManifestHoldoutRatio(dicts, name),
        };
        if (inputs.plaintext == 0 && inputs.stored == 0) continue;
        if (ShouldRecommendMoreSamples(inputs)) {
            recommendations.push_back(FormatCompressionRecommendation(inputs));
        }
    }
    return recommendations;
}

} // namespace

bool DictBootstrapManager::SaveCompressionReport() const
{
    const fs::path report_path{m_datadir / "swords" / "compression_report.json"};
    fs::create_directories(report_path.parent_path());

    const int64_t pass2_wall_seconds{
        m_metrics.pass2_end != std::chrono::steady_clock::time_point{}
            ? Ticks<std::chrono::seconds>(m_metrics.pass2_end - m_metrics.pass2_start)
            : 0};

    std::array<uint64_t, NUM_BLOCK_BUCKETS> baseline_block_plaintext{};
    std::array<uint64_t, NUM_UTXO_BUCKETS> baseline_utxo_plaintext{};
    uint64_t total_baseline_block_plaintext{0};
    uint64_t total_baseline_utxo_plaintext{0};
    int64_t pass1_wall_seconds{0};
    const fs::path baseline_path{m_datadir / "swords" / "bootstrap_baseline.json"};
    if (fs::exists(baseline_path)) {
        std::ifstream in{baseline_path};
        UniValue baseline;
        if (in.is_open()
            && baseline.read(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()})
            && baseline.isObject()) {
            if (baseline["pass1_wall_seconds"].isNum()) {
                pass1_wall_seconds = baseline["pass1_wall_seconds"].getInt<int64_t>();
            }
            if (baseline["block_plaintext_bytes"].isObject()) {
                for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
                    const std::string name{BlockBucketName(static_cast<BlockBucket>(i))};
                    if (baseline["block_plaintext_bytes"][name].isNum()) {
                        baseline_block_plaintext[i] = baseline["block_plaintext_bytes"][name].getInt<uint64_t>();
                        total_baseline_block_plaintext += baseline_block_plaintext[i];
                    }
                }
            }
            if (baseline["utxo_plaintext_bytes"].isObject()) {
                for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
                    const std::string name{UtxoBucketName(static_cast<UtxoBucket>(i))};
                    if (baseline["utxo_plaintext_bytes"][name].isNum()) {
                        baseline_utxo_plaintext[i] = baseline["utxo_plaintext_bytes"][name].getInt<uint64_t>();
                        total_baseline_utxo_plaintext += baseline_utxo_plaintext[i];
                    }
                }
            }
        }
    }

    uint64_t pass2_block_stored{0};
    uint64_t pass2_utxo_stored{0};
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        pass2_block_stored += m_metrics.block_stored_bytes[i].load();
    }
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        pass2_utxo_stored += m_metrics.utxo_stored_bytes[i].load();
    }

    UniValue json{UniValue::VOBJ};
    json.pushKV("state", "complete");
    json.pushKV("pass1_wall_seconds", pass1_wall_seconds);
    json.pushKV("pass2_wall_seconds", pass2_wall_seconds);

    UniValue block_buckets{UniValue::VARR};
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        const auto bucket{static_cast<BlockBucket>(i)};
        const std::string name{BlockBucketName(bucket)};
        const uint64_t plaintext{baseline_block_plaintext[i]};
        const uint64_t stored{m_metrics.block_stored_bytes[i].load()};
        if (plaintext == 0 && stored == 0) continue;

        UniValue entry{UniValue::VOBJ};
        entry.pushKV("name", name);
        if (const auto dict_size{ManifestDictSize(Dicts(), name)}) {
            entry.pushKV("dict_size", static_cast<int64_t>(*dict_size));
        }
        if (const auto ratio{ManifestHoldoutRatio(Dicts(), name)}) {
            entry.pushKV("holdout_ratio", *ratio);
        }
        if (plaintext > 0 && stored > 0) {
            entry.pushKV("compression_ratio", static_cast<double>(plaintext) / static_cast<double>(stored));
        }
        entry.pushKV("plaintext_bytes", static_cast<int64_t>(plaintext));
        entry.pushKV("stored_bytes", static_cast<int64_t>(stored));
        if (plaintext > 0) {
            entry.pushKV("savings_percent", SavingsPercent(plaintext, stored));
        }
        block_buckets.push_back(entry);
    }
    json.pushKV("block_buckets", block_buckets);

    UniValue utxo_buckets{UniValue::VARR};
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        const auto bucket{static_cast<UtxoBucket>(i)};
        const std::string name{UtxoBucketName(bucket)};
        const uint64_t plaintext{baseline_utxo_plaintext[i]};
        const uint64_t stored{m_metrics.utxo_stored_bytes[i].load()};
        if (plaintext == 0 && stored == 0) continue;

        UniValue entry{UniValue::VOBJ};
        entry.pushKV("name", name);
        if (const auto dict_size{ManifestDictSize(Dicts(), name)}) {
            entry.pushKV("dict_size", static_cast<int64_t>(*dict_size));
        }
        if (const auto ratio{ManifestHoldoutRatio(Dicts(), name)}) {
            entry.pushKV("holdout_ratio", *ratio);
        }
        if (plaintext > 0 && stored > 0) {
            entry.pushKV("compression_ratio", static_cast<double>(plaintext) / static_cast<double>(stored));
        }
        entry.pushKV("plaintext_bytes", static_cast<int64_t>(plaintext));
        entry.pushKV("stored_bytes", static_cast<int64_t>(stored));
        if (plaintext > 0) {
            entry.pushKV("savings_percent", SavingsPercent(plaintext, stored));
        }
        utxo_buckets.push_back(entry);
    }
    json.pushKV("utxo_buckets", utxo_buckets);

    UniValue global{UniValue::VOBJ};
    global.pushKV("blocks_plaintext_pass1", static_cast<int64_t>(total_baseline_block_plaintext));
    global.pushKV("blocks_stored_pass2", static_cast<int64_t>(pass2_block_stored));
    global.pushKV("utxo_plaintext_pass1", static_cast<int64_t>(total_baseline_utxo_plaintext));
    global.pushKV("utxo_stored_pass2", static_cast<int64_t>(pass2_utxo_stored));
    if (total_baseline_block_plaintext > 0) {
        global.pushKV("blocks_savings_percent", SavingsPercent(total_baseline_block_plaintext, pass2_block_stored));
    }
    if (total_baseline_utxo_plaintext > 0) {
        global.pushKV("utxo_savings_percent", SavingsPercent(total_baseline_utxo_plaintext, pass2_utxo_stored));
    }
    json.pushKV("global", global);

    const std::vector<std::string> recommendations{
        CollectCompressionRecommendations(Dicts(), baseline_block_plaintext, baseline_utxo_plaintext, m_metrics)};
    if (!recommendations.empty()) {
        UniValue recs{UniValue::VARR};
        for (const std::string& recommendation : recommendations) {
            recs.push_back(recommendation);
        }
        json.pushKV("recommendations", recs);
    }

    const fs::path tmp{fs::u8path(fs::PathToString(report_path) + ".tmp")};
    std::ofstream out{tmp};
    if (!out.is_open()) return false;
    out << json.write(4, 1) << std::endl;
    out.close();
    std::error_code ec;
    fs::rename(tmp, report_path, ec);
    if (ec) return false;
    LogPrintf("Dictionary bootstrap compression report written to %s\n", fs::PathToString(report_path));
    return true;
}

void DictBootstrapManager::CompletePass2()
{
    m_metrics.pass2_end = std::chrono::steady_clock::now();
    std::array<uint64_t, NUM_BLOCK_BUCKETS> baseline_block_plaintext{};
    std::array<uint64_t, NUM_UTXO_BUCKETS> baseline_utxo_plaintext{};
    int64_t pass1_wall_seconds{0};
    const fs::path baseline_path{m_datadir / "swords" / "bootstrap_baseline.json"};
    if (fs::exists(baseline_path)) {
        std::ifstream in{baseline_path};
        UniValue baseline;
        if (in.is_open()
            && baseline.read(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()})
            && baseline.isObject()) {
            if (baseline["pass1_wall_seconds"].isNum()) {
                pass1_wall_seconds = baseline["pass1_wall_seconds"].getInt<int64_t>();
            }
            if (baseline["block_plaintext_bytes"].isObject()) {
                for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
                    const std::string name{BlockBucketName(static_cast<BlockBucket>(i))};
                    if (baseline["block_plaintext_bytes"][name].isNum()) {
                        baseline_block_plaintext[i] = baseline["block_plaintext_bytes"][name].getInt<uint64_t>();
                    }
                }
            }
            if (baseline["utxo_plaintext_bytes"].isObject()) {
                for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
                    const std::string name{UtxoBucketName(static_cast<UtxoBucket>(i))};
                    if (baseline["utxo_plaintext_bytes"][name].isNum()) {
                        baseline_utxo_plaintext[i] = baseline["utxo_plaintext_bytes"][name].getInt<uint64_t>();
                    }
                }
            }
        }
    }
    if (!SaveCompressionReport()) {
        InitWarning(Untranslated(strprintf(
            "Dictionary bootstrap pass 2 finished but failed to persist compression report to %s.",
            fs::PathToString(m_datadir / "swords" / "compression_report.json"))));
    }
    m_phase = BootstrapPhase::COMPLETE;
    if (!SaveState()) {
        InitWarning(Untranslated(strprintf(
            "Dictionary bootstrap pass 2 finished but failed to persist state to %s.",
            fs::PathToString(m_state_path))));
    }

    const int64_t pass2_seconds{Ticks<std::chrono::seconds>(m_metrics.pass2_end - m_metrics.pass2_start)};
    LogPrintf("=== Swords dictionary effectiveness report (pass 2) ===\n");
    LogPrintf("Block buckets:\n");
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        const auto bucket{static_cast<BlockBucket>(i)};
        const std::string name{BlockBucketName(bucket)};
        const uint64_t plaintext{baseline_block_plaintext[i]};
        const uint64_t stored{m_metrics.block_stored_bytes[i].load()};
        if (plaintext == 0 && stored == 0) continue;
        const auto dict_size{ManifestDictSize(Dicts(), name)};
        const auto holdout{ManifestHoldoutRatio(Dicts(), name)};
        const double compression_ratio{stored > 0 ? static_cast<double>(plaintext) / static_cast<double>(stored) : 0.0};
        if (holdout) {
            LogPrintf("  %s: dict=%s ratio=%.2f holdout=%.2f plaintext=%s stored=%s saved=%d%%\n",
                      name,
                      dict_size ? FormatKiB(*dict_size) : "n/a",
                      compression_ratio,
                      *holdout,
                      FormatHumanBytes(plaintext).c_str(),
                      FormatHumanBytes(stored).c_str(),
                      SavingsPercent(plaintext, stored));
        } else {
            LogPrintf("  %s: dict=%s ratio=%.2f holdout=n/a plaintext=%s stored=%s saved=%d%%\n",
                      name,
                      dict_size ? FormatKiB(*dict_size) : "n/a",
                      compression_ratio,
                      FormatHumanBytes(plaintext).c_str(),
                      FormatHumanBytes(stored).c_str(),
                      SavingsPercent(plaintext, stored));
        }
    }
    LogPrintf("UTXO buckets:\n");
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        const auto bucket{static_cast<UtxoBucket>(i)};
        const std::string name{UtxoBucketName(bucket)};
        const uint64_t plaintext{baseline_utxo_plaintext[i]};
        const uint64_t stored{m_metrics.utxo_stored_bytes[i].load()};
        if (plaintext == 0 && stored == 0) continue;
        const auto dict_size{ManifestDictSize(Dicts(), name)};
        const auto holdout{ManifestHoldoutRatio(Dicts(), name)};
        const double compression_ratio{stored > 0 ? static_cast<double>(plaintext) / static_cast<double>(stored) : 0.0};
        if (holdout) {
            LogPrintf("  %s: dict=%s ratio=%.2f holdout=%.2f plaintext=%s stored=%s saved=%d%%\n",
                      name,
                      dict_size ? FormatKiB(*dict_size) : "n/a",
                      compression_ratio,
                      *holdout,
                      FormatHumanBytes(plaintext).c_str(),
                      FormatHumanBytes(stored).c_str(),
                      SavingsPercent(plaintext, stored));
        } else {
            LogPrintf("  %s: dict=%s ratio=%.2f holdout=n/a plaintext=%s stored=%s saved=%d%%\n",
                      name,
                      dict_size ? FormatKiB(*dict_size) : "n/a",
                      compression_ratio,
                      FormatHumanBytes(plaintext).c_str(),
                      FormatHumanBytes(stored).c_str(),
                      SavingsPercent(plaintext, stored));
        }
    }
    uint64_t total_block_plaintext{0};
    uint64_t total_block_stored{0};
    uint64_t total_utxo_plaintext{0};
    uint64_t total_utxo_stored{0};
    for (size_t i = 0; i < NUM_BLOCK_BUCKETS; ++i) {
        total_block_plaintext += baseline_block_plaintext[i];
        total_block_stored += m_metrics.block_stored_bytes[i].load();
    }
    for (size_t i = 0; i < NUM_UTXO_BUCKETS; ++i) {
        total_utxo_plaintext += baseline_utxo_plaintext[i];
        total_utxo_stored += m_metrics.utxo_stored_bytes[i].load();
    }
    LogPrintf("Global:\n");
    LogPrintf("  blocks_plaintext_pass1=%s blocks_stored_pass2=%s savings=%d%%\n",
              FormatHumanBytes(total_block_plaintext).c_str(),
              FormatHumanBytes(total_block_stored).c_str(),
              SavingsPercent(total_block_plaintext, total_block_stored));
    LogPrintf("  utxo_plaintext_pass1=%s utxo_stored_pass2=%s savings=%d%%\n",
              FormatHumanBytes(total_utxo_plaintext).c_str(),
              FormatHumanBytes(total_utxo_stored).c_str(),
              SavingsPercent(total_utxo_plaintext, total_utxo_stored));
    LogPrintf("  pass1_ibd_hours=%.2f pass2_reindex_hours=%.2f\n",
              static_cast<double>(pass1_wall_seconds) / 3600.0,
              static_cast<double>(pass2_seconds) / 3600.0);
    const std::vector<std::string> recommendations{
        CollectCompressionRecommendations(Dicts(), baseline_block_plaintext, baseline_utxo_plaintext, m_metrics)};
    if (!recommendations.empty()) {
        LogPrintf("Recommendations:\n");
        for (const std::string& recommendation : recommendations) {
            LogPrintf("  %s\n", recommendation);
        }
    }
    LogPrintf("Dictionary bootstrap pass 2 complete; typed dictionary compression active\n");
}

void DictBootstrapManager::OnReindexComplete()
{
    if (m_phase != BootstrapPhase::PASS2_IN_PROGRESS) return;
    CompletePass2();
}

bool InitDictBootstrap(const fs::path& datadir, const CChainParams& params, const ArgsManager& args, const bool do_reindex)
{
    if (!DictBootstrapManager::IsBootstrapEnabledForChain(params)) {
        if (args.IsArgSet("-dictbootstrap")) {
            InitWarning(Untranslated("Typed dictionary bootstrap is mainnet-only and has been disabled on this chain."));
        }
        return true;
    }

    const std::string mode{args.GetArg("-dictbootstrap", "auto")};
    if (!DictBootstrapManager::IsValidBootstrapMode(mode)) {
        return InitError(strprintf(_("Invalid -dictbootstrap value '%s' (allowed: auto, off)"), mode));
    }

    g_dict_bootstrap = std::make_unique<DictBootstrapManager>(datadir, params, args);
    if (do_reindex && g_dict_bootstrap->Phase() == BootstrapPhase::PASS1_COMPLETE) {
        g_dict_bootstrap->EnterPass2();
    }
    return true;
}

void ShutdownDictBootstrap()
{
    g_dict_bootstrap.reset();
}

} // namespace compress
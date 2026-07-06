// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compress/zstd.h>

#include <logging.h>
#include <random.h>
#include <streams.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/fs_helpers.h>

#define ZDICT_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zdict.h>

#include <algorithm>
#include <cinttypes>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

namespace compress {
namespace {

struct ZstdDeleter {
    void operator()(ZSTD_CDict* ptr) const { ZSTD_freeCDict(ptr); }
    void operator()(ZSTD_DDict* ptr) const { ZSTD_freeDDict(ptr); }
    void operator()(ZSTD_CCtx* ptr) const { ZSTD_freeCCtx(ptr); }
    void operator()(ZSTD_DCtx* ptr) const { ZSTD_freeDCtx(ptr); }
};

static constexpr std::array<size_t, 8> TRAIN_CAPACITY_CANDIDATES_KIB{32, 64, 128, 256, 512, 1024, 2048, 4096};
static constexpr double HOLDOUT_RATIO_TIE_TOLERANCE{0.005};

size_t TotalSampleBytes(std::span<const std::span<const uint8_t>> samples)
{
    size_t total{0};
    for (const auto& sample : samples) {
        total += sample.size();
    }
    return total;
}

std::vector<uint8_t> ConcatSamples(std::span<const std::span<const uint8_t>> samples,
                                   std::vector<size_t>& sample_sizes)
{
    sample_sizes.clear();
    sample_sizes.reserve(samples.size());
    size_t total{0};
    for (const auto& sample : samples) {
        sample_sizes.push_back(sample.size());
        total += sample.size();
    }
    std::vector<uint8_t> buffer(total);
    size_t offset{0};
    for (const auto& sample : samples) {
        if (sample.empty()) continue;
        std::copy(sample.begin(), sample.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += sample.size();
    }
    return buffer;
}

double MeasureHoldoutRatio(const std::vector<uint8_t>& dict,
                           std::span<const std::span<const uint8_t>> holdout_samples)
{
    if (dict.empty() || holdout_samples.empty()) return 0.0;

    DictZstd zstd{dict};
    if (!zstd) return 0.0;

    size_t plaintext{0};
    size_t compressed{0};
    std::vector<uint8_t> out;
    for (const auto& sample : holdout_samples) {
        if (sample.empty()) continue;
        plaintext += sample.size();
        if (!zstd.Compress(sample, out, ZSTD_defaultCLevel())) return 0.0;
        compressed += out.size();
    }
    if (compressed == 0) return 0.0;
    return static_cast<double>(plaintext) / static_cast<double>(compressed);
}

std::optional<std::vector<uint8_t>> TrainAtCapacity(std::span<const std::span<const uint8_t>> train_samples,
                                                    size_t capacity_bytes)
{
    std::vector<size_t> sample_sizes;
    const std::vector<uint8_t> sample_buffer{ConcatSamples(train_samples, sample_sizes)};
    if (sample_buffer.empty() || sample_sizes.empty()) return std::nullopt;

    std::vector<uint8_t> dict(capacity_bytes);
    ZDICT_cover_params_t params{};
    params.k = 50;
    params.d = 8;
    params.steps = 4;
    params.splitPoint = 1.0;
    params.shrinkDict = 1;
    params.shrinkDictMaxRegression = 1;
    params.zParams.notificationLevel = 0;

    const size_t dict_size{ZDICT_optimizeTrainFromBuffer_cover(
        dict.data(), dict.capacity(), sample_buffer.data(), sample_sizes.data(),
        static_cast<unsigned>(sample_sizes.size()), &params)};
    if (ZDICT_isError(dict_size)) {
        return std::nullopt;
    }
    dict.resize(dict_size);
    return dict;
}

bool ParseManifest(const fs::path& path, std::vector<DictManifestEntry>& entries)
{
    entries.clear();
    if (!fs::exists(path)) return false;

    std::ifstream file{path};
    if (!file.is_open()) return false;
    const std::string contents{std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}};
    UniValue json;
    if (!json.read(contents) || !json.isObject()) return false;

    const UniValue& buckets{json["buckets"]};
    if (!buckets.isArray()) return false;
    for (size_t i = 0; i < buckets.size(); ++i) {
        const UniValue& entry{buckets[i]};
        if (!entry.isObject()) continue;
        DictManifestEntry manifest_entry;
        manifest_entry.name = entry["name"].isStr() ? entry["name"].get_str() : "";
        manifest_entry.kind = entry["kind"].isStr() ? entry["kind"].get_str() : "";
        manifest_entry.bucket_id = entry["bucket_id"].isNum() ? static_cast<uint8_t>(entry["bucket_id"].getInt<int>()) : 0;
        manifest_entry.filename = entry["filename"].isStr() ? entry["filename"].get_str() : "";
        if (entry["chosen_size"].isNum()) {
            manifest_entry.chosen_size = static_cast<size_t>(entry["chosen_size"].getInt<int64_t>());
        }
        if (entry["holdout_ratio"].isNum()) {
            manifest_entry.holdout_ratio = entry["holdout_ratio"].get_real();
        }
        if (!manifest_entry.filename.empty()) {
            entries.push_back(std::move(manifest_entry));
        }
    }
    return !entries.empty();
}

} // namespace

struct DictZstd::Impl
{
    std::vector<uint8_t> dictionary;
    std::unique_ptr<ZSTD_CDict, ZstdDeleter> cdict;
    std::unique_ptr<ZSTD_DDict, ZstdDeleter> ddict;

    explicit Impl(std::vector<uint8_t> dict)
        : dictionary{std::move(dict)}
    {
        cdict.reset(ZSTD_createCDict(dictionary.data(), dictionary.size(), ZSTD_defaultCLevel()));
        ddict.reset(ZSTD_createDDict(dictionary.data(), dictionary.size()));
        if (!cdict || !ddict) {
            throw std::runtime_error{"Failed to initialize zstd dictionary"};
        }
    }
};

std::optional<std::vector<uint8_t>> LoadDictionaryFile(const fs::path& path)
{
    if (path.empty() || !fs::exists(path)) {
        return std::nullopt;
    }

    FILE* file{fsbridge::fopen(path, "rb")};
    if (file == nullptr) {
        LogPrintf("Failed to open zstd dictionary '%s'\n", fs::PathToString(path));
        return std::nullopt;
    }

    AutoFile dict_file{file};
    const auto file_size_u{fs::file_size(path)};
    const int64_t file_size{file_size_u > static_cast<uintmax_t>(std::numeric_limits<int64_t>::max())
                               ? std::numeric_limits<int64_t>::max()
                               : static_cast<int64_t>(file_size_u)};
    if (file_size <= 0 || static_cast<size_t>(file_size) > MAX_DICT_FILE_SIZE) {
        LogWarning("Unexpected zstd dictionary size %" PRId64 " for '%s' (max %u)\n",
                   file_size, fs::PathToString(path), MAX_DICT_FILE_SIZE);
        return std::nullopt;
    }

    if (static_cast<size_t>(file_size) > DICT_SIZE_WARN_THRESHOLD) {
        LogWarning("Large zstd dictionary (%" PRId64 " bytes) loaded from '%s'\n",
                   file_size, fs::PathToString(path));
    }

    std::vector<uint8_t> dictionary(static_cast<size_t>(file_size));
    try {
        dict_file.read(MakeWritableByteSpan(dictionary));
    } catch (const std::exception& e) {
        LogWarning("Failed to read zstd dictionary '%s': %s\n", fs::PathToString(path), e.what());
        return std::nullopt;
    }
    if (dict_file.fclose() != 0) {
        LogWarning("Failed to close zstd dictionary '%s'\n", fs::PathToString(path));
        return std::nullopt;
    }

    return dictionary;
}

std::optional<TrainDictionaryResult> TrainDictionary(std::span<const std::span<const uint8_t>> samples)
{
    if (samples.size() < 2) return std::nullopt;

    std::vector<size_t> order(samples.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    FastRandomContext rng;
    for (size_t i = order.size(); i > 1; --i) {
        const size_t j{rng.randrange(i)};
        std::swap(order[i - 1], order[j]);
    }

    std::vector<std::span<const uint8_t>> shuffled;
    shuffled.reserve(samples.size());
    for (const size_t idx : order) {
        shuffled.emplace_back(samples[idx]);
    }

    const size_t split_idx{std::max<size_t>(1, shuffled.size() * 9 / 10)};
    if (split_idx >= shuffled.size()) return std::nullopt;

    const auto train_samples{std::span<const std::span<const uint8_t>>{shuffled}.subspan(0, split_idx)};
    const auto holdout_samples{std::span<const std::span<const uint8_t>>{shuffled}.subspan(split_idx)};
    if (train_samples.empty() || holdout_samples.empty()) return std::nullopt;

    TrainDictionaryResult best;
    double best_ratio{0.0};

    for (const size_t capacity_kib : TRAIN_CAPACITY_CANDIDATES_KIB) {
        const size_t capacity{capacity_kib * 1024};
        auto dict_opt{TrainAtCapacity(train_samples, capacity)};
        if (!dict_opt) continue;

        const double ratio{MeasureHoldoutRatio(*dict_opt, holdout_samples)};
        best.candidates_tried.push_back(capacity);

        const bool better{ratio > best_ratio * (1.0 + HOLDOUT_RATIO_TIE_TOLERANCE)};
        const bool tie_prefer_smaller{std::abs(ratio - best_ratio) <= best_ratio * HOLDOUT_RATIO_TIE_TOLERANCE
                                      && dict_opt->size() < best.chosen_size};
        if (best.chosen_size == 0 || better || tie_prefer_smaller) {
            best_ratio = ratio;
            best.dictionary = std::move(*dict_opt);
            best.chosen_size = best.dictionary.size();
            best.holdout_ratio = ratio;
        }
    }

    if (best.dictionary.empty()) return std::nullopt;
    best.sample_bytes = TotalSampleBytes(samples);
    return best;
}

fs::path DefaultBlockDictionaryPath()
{
#ifdef BLOCK_ZSTD_DICT_INSTALL_PATH
    const fs::path install_path{fs::u8path(BLOCK_ZSTD_DICT_INSTALL_PATH)};
    if (fs::exists(install_path)) {
        return install_path;
    }
#endif
#ifdef BLOCK_ZSTD_DICT_SOURCE_PATH
    return fs::u8path(BLOCK_ZSTD_DICT_SOURCE_PATH);
#else
    return {};
#endif
}

fs::path DefaultUtxoDictionaryPath()
{
#ifdef UTXO_ZSTD_DICT_INSTALL_PATH
    const fs::path install_path{fs::u8path(UTXO_ZSTD_DICT_INSTALL_PATH)};
    if (fs::exists(install_path)) {
        return install_path;
    }
#endif
#ifdef UTXO_ZSTD_DICT_SOURCE_PATH
    return fs::u8path(UTXO_ZSTD_DICT_SOURCE_PATH);
#else
    return {};
#endif
}

fs::path DefaultDictManifestPath()
{
#ifdef DICT_MANIFEST_INSTALL_PATH
    const fs::path install_path{fs::u8path(DICT_MANIFEST_INSTALL_PATH)};
    if (fs::exists(install_path)) {
        return install_path;
    }
#endif
#ifdef DICT_MANIFEST_SOURCE_PATH
    return fs::u8path(DICT_MANIFEST_SOURCE_PATH);
#else
    return {};
#endif
}

fs::path DefaultTypedDictsDir()
{
#ifdef TYPED_DICTS_INSTALL_DIR
    const fs::path install_path{fs::u8path(TYPED_DICTS_INSTALL_DIR)};
    if (fs::exists(install_path)) {
        return install_path;
    }
#endif
#ifdef TYPED_DICTS_SOURCE_DIR
    return fs::u8path(TYPED_DICTS_SOURCE_DIR);
#else
    return {};
#endif
}

fs::path DictSet::ResolveDictsDir(const fs::path& datadir)
{
    const fs::path datadir_dicts{datadir / "swords" / "dicts"};
    if (fs::exists(datadir_dicts)) {
        return datadir_dicts;
    }
    return DefaultTypedDictsDir();
}

fs::path DictSet::ResolveManifestPath(const fs::path& datadir)
{
    const fs::path datadir_manifest{datadir / "swords" / "dict_manifest.json"};
    if (fs::exists(datadir_manifest)) {
        return datadir_manifest;
    }
    return DefaultDictManifestPath();
}

bool DictSet::Load(const fs::path& datadir, const fs::path& manifest_path)
{
    m_entries.clear();
    m_block_dicts = {};
    m_utxo_dicts = {};

    const fs::path resolved_manifest{manifest_path.empty() ? ResolveManifestPath(datadir) : manifest_path};
    if (!ParseManifest(resolved_manifest, m_entries)) {
        LogPrintf("Failed to load dictionary manifest from '%s'\n", fs::PathToString(resolved_manifest));
        return false;
    }

    const fs::path dicts_dir{ResolveDictsDir(datadir)};
    for (const DictManifestEntry& entry : m_entries) {
        const fs::path dict_path{dicts_dir / fs::u8path(entry.filename)};
        const auto dict_bytes{LoadDictionaryFile(dict_path)};
        if (!dict_bytes) {
            LogWarning("Missing typed dictionary '%s' for bucket %s; compression for this bucket will be skipped\n",
                       fs::PathToString(dict_path), entry.name);
            continue;
        }
        if (entry.kind == "block" && entry.bucket_id < NUM_BLOCK_BUCKETS) {
            m_block_dicts[entry.bucket_id] = DictZstd{*dict_bytes};
        } else if (entry.kind == "utxo" && entry.bucket_id < NUM_UTXO_BUCKETS) {
            m_utxo_dicts[entry.bucket_id] = DictZstd{*dict_bytes};
        }
    }
    return true;
}

const DictZstd& DictSet::BlockDict(BlockBucket bucket) const
{
    return m_block_dicts[static_cast<size_t>(bucket)];
}

const DictZstd& DictSet::UtxoDict(UtxoBucket bucket) const
{
    return m_utxo_dicts[static_cast<size_t>(bucket)];
}

DictZstd::DictZstd(std::vector<uint8_t> dictionary)
{
    if (dictionary.empty()) return;
    try {
        m_impl = std::make_unique<Impl>(std::move(dictionary));
    } catch (const std::exception& e) {
        LogWarning("Failed to load zstd dictionary: %s\n", e.what());
    }
}

DictZstd::DictZstd() = default;

DictZstd::DictZstd(DictZstd&&) noexcept = default;

DictZstd& DictZstd::operator=(DictZstd&&) noexcept = default;

DictZstd::~DictZstd() = default;

bool DictZstd::Compress(std::span<const uint8_t> input, std::vector<uint8_t>& output, int level) const
{
    if (!m_impl) return false;
    level = std::clamp(level, MIN_LEVEL, MAX_LEVEL);

    std::unique_ptr<ZSTD_CCtx, ZstdDeleter> ctx{ZSTD_createCCtx()};
    if (!ctx) return false;
    if (ZSTD_isError(ZSTD_CCtx_refCDict(ctx.get(), m_impl->cdict.get()))) return false;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_compressionLevel, level))) return false;

    const size_t bound{ZSTD_compressBound(input.size())};
    output.resize(bound);

    const size_t written{ZSTD_compress2(ctx.get(), output.data(), output.size(), input.data(), input.size())};

    if (ZSTD_isError(written)) {
        LogPrintf("zstd compress failed: %s\n", ZSTD_getErrorName(written));
        return false;
    }

    output.resize(written);
    return true;
}

bool DictZstd::Decompress(std::span<const uint8_t> input, std::vector<uint8_t>& output, size_t max_output_size) const
{
    if (!m_impl) return false;

    const unsigned long long declared{ZSTD_getFrameContentSize(input.data(), input.size())};
    if (declared == ZSTD_CONTENTSIZE_UNKNOWN || declared == ZSTD_CONTENTSIZE_ERROR) {
        LogPrintf("zstd decompress failed: unknown compressed frame size\n");
        return false;
    }
    if (declared > max_output_size) {
        LogPrintf("zstd decompress failed: frame size %llu exceeds limit %u\n", declared, max_output_size);
        return false;
    }

    std::unique_ptr<ZSTD_DCtx, ZstdDeleter> ctx{ZSTD_createDCtx()};
    if (!ctx) return false;
    if (ZSTD_isError(ZSTD_DCtx_refDDict(ctx.get(), m_impl->ddict.get()))) return false;

    output.resize(static_cast<size_t>(declared));
    const size_t written{ZSTD_decompressDCtx(ctx.get(), output.data(), output.size(), input.data(), input.size())};

    if (ZSTD_isError(written)) {
        LogPrintf("zstd decompress failed: %s\n", ZSTD_getErrorName(written));
        return false;
    }

    output.resize(written);
    return true;
}

} // namespace compress
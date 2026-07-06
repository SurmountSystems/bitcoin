// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMPRESS_ZSTD_H
#define BITCOIN_COMPRESS_ZSTD_H

#include <compress/dict_classify.h>
#include <span.h>
#include <util/fs.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <optional>
#include <string>
#include <vector>

namespace compress {

static constexpr size_t MAX_DICT_FILE_SIZE{4 * 1024 * 1024};
static constexpr size_t DICT_SIZE_WARN_THRESHOLD{1024 * 1024};

/** Load a zstd dictionary from disk. Returns nullopt on failure. */
std::optional<std::vector<uint8_t>> LoadDictionaryFile(const fs::path& path);

struct TrainDictionaryResult {
    std::vector<uint8_t> dictionary;
    size_t chosen_size{0};
    double holdout_ratio{0.0};
    std::vector<size_t> candidates_tried;
    size_t sample_bytes{0};
};

/**
 * Train a dictionary from sample buffers using cover optimization with holdout.
 * Returns nullopt when training fails or samples are insufficient.
 */
std::optional<TrainDictionaryResult> TrainDictionary(std::span<const std::span<const uint8_t>> samples);

/**
 * zstd dictionary-backed compressor/decompressor.
 * Thread-safe for concurrent compress/decompress calls after construction.
 */
class DictZstd
{
public:
    static constexpr int MIN_LEVEL{1};
    static constexpr int MAX_LEVEL{22};

    DictZstd();
    explicit DictZstd(std::vector<uint8_t> dictionary);
    DictZstd(DictZstd&&) noexcept;
    DictZstd& operator=(DictZstd&&) noexcept;
    DictZstd(const DictZstd&) = delete;
    DictZstd& operator=(const DictZstd&) = delete;

    /** True when a dictionary was loaded successfully. */
    explicit operator bool() const { return static_cast<bool>(m_impl); }

    /**
     * Compress input using the configured level and dictionary.
     * Returns false if compression is unavailable or fails.
     */
    bool Compress(std::span<const uint8_t> input, std::vector<uint8_t>& output, int level) const;

    /**
     * Decompress input into output, resizing output to the decompressed size.
     * Returns false on error. max_output_size bounds allocation (DoS protection).
     */
    bool Decompress(std::span<const uint8_t> input, std::vector<uint8_t>& output, size_t max_output_size) const;

    ~DictZstd();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

using BlockZstd = DictZstd;
using UtxoZstd = DictZstd;

struct DictManifestEntry {
    std::string name;
    std::string kind; // "block" or "utxo"
    uint8_t bucket_id{0};
    std::string filename;
    std::optional<size_t> chosen_size;
    std::optional<double> holdout_ratio;
};

/** Per-bucket dictionary set loaded from manifest + dict files. */
class DictSet
{
public:
    DictSet() = default;

    /** Load dictionaries using datadir/swords/dicts/ then bundled share/swords/dicts/. */
    bool Load(const fs::path& datadir, const fs::path& manifest_path = {});

    const DictZstd& BlockDict(BlockBucket bucket) const;
    const DictZstd& UtxoDict(UtxoBucket bucket) const;

    const std::vector<DictManifestEntry>& ManifestEntries() const { return m_entries; }

    /** Resolve manifest path: datadir override, then bundled. */
    static fs::path ResolveManifestPath(const fs::path& datadir);
    static fs::path ResolveDictsDir(const fs::path& datadir);

private:
    std::array<DictZstd, NUM_BLOCK_BUCKETS> m_block_dicts{};
    std::array<DictZstd, NUM_UTXO_BUCKETS> m_utxo_dicts{};
    std::vector<DictManifestEntry> m_entries;
};

/** Resolve the default bundled block dictionary path for this build/install. */
fs::path DefaultBlockDictionaryPath();

/** Resolve the default bundled UTXO dictionary path for this build/install. */
fs::path DefaultUtxoDictionaryPath();

/** Resolve bundled share/swords/dict_manifest.json. */
fs::path DefaultDictManifestPath();

/** Resolve bundled share/swords/dicts/ directory. */
fs::path DefaultTypedDictsDir();

} // namespace compress

#endif // BITCOIN_COMPRESS_ZSTD_H
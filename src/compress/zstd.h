// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMPRESS_ZSTD_H
#define BITCOIN_COMPRESS_ZSTD_H

#include <span.h>
#include <util/fs.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <optional>
#include <string>
#include <vector>

namespace compress {

/** Load a zstd dictionary from disk. Returns nullopt on failure. */
std::optional<std::vector<uint8_t>> LoadDictionaryFile(const fs::path& path);

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

/** Resolve the default bundled block dictionary path for this build/install. */
fs::path DefaultBlockDictionaryPath();

/** Resolve the default bundled UTXO dictionary path for this build/install. */
fs::path DefaultUtxoDictionaryPath();

} // namespace compress

#endif // BITCOIN_COMPRESS_ZSTD_H
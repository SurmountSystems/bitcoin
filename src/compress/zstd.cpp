// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compress/zstd.h>

#include <logging.h>
#include <streams.h>
#include <util/fs.h>
#include <util/fs_helpers.h>

#include <zstd.h>

#include <algorithm>
#include <cinttypes>
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

} // namespace

struct BlockZstd::Impl
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
    const int64_t file_size{fs::file_size(path)};
    if (file_size <= 0 || file_size > 1024 * 1024) {
        LogWarning("Unexpected zstd dictionary size %" PRId64 " for '%s'\n", file_size, fs::PathToString(path));
        return std::nullopt;
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

BlockZstd::BlockZstd(std::vector<uint8_t> dictionary)
{
    if (dictionary.empty()) return;
    try {
        m_impl = std::make_unique<Impl>(std::move(dictionary));
    } catch (const std::exception& e) {
        LogWarning("Failed to load zstd dictionary: %s\n", e.what());
    }
}

BlockZstd::BlockZstd() = default;

BlockZstd::BlockZstd(BlockZstd&&) noexcept = default;

BlockZstd& BlockZstd::operator=(BlockZstd&&) noexcept = default;

BlockZstd::~BlockZstd() = default;

bool BlockZstd::Compress(std::span<const uint8_t> input, std::vector<uint8_t>& output, int level) const
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

bool BlockZstd::Decompress(std::span<const uint8_t> input, std::vector<uint8_t>& output, size_t max_output_size) const
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
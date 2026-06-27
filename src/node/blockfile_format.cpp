// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockfile_format.h>

#include <consensus/consensus.h>
#include <kernel/chainparams.h>

namespace node {
namespace {

uint32_t ReadLE32(Span<const uint8_t> bytes)
{
    if (bytes.size() < 4) return 0;
    return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) | (uint32_t{bytes[2]} << 16) | (uint32_t{bytes[3]} << 24);
}

bool ValidUncompressedStoredSize(uint32_t size)
{
    return size >= 80 && size <= MAX_BLOCK_SERIALIZED_SIZE;
}

bool ValidCompressedStoredSize(uint32_t size)
{
    return size > 0 && size <= MAX_BLOCK_SERIALIZED_SIZE;
}

bool MatchesMagic(const CChainParams& params, Span<const uint8_t> bytes)
{
    if (bytes.size() < 4) return false;
    const auto& expected{params.MessageStart()};
    return bytes[0] == uint8_t{expected[0]} && bytes[1] == uint8_t{expected[1]} &&
           bytes[2] == uint8_t{expected[2]} && bytes[3] == uint8_t{expected[3]};
}

Span<const uint8_t> BytesAtOffset(Span<const uint8_t> bytes_before_payload, const uint32_t payload_pos,
                                  const uint32_t file_offset, const uint32_t length)
{
    const uint32_t bytes_start{payload_pos - static_cast<uint32_t>(bytes_before_payload.size())};
    if (file_offset < bytes_start) return {};
    const uint32_t rel_start{file_offset - bytes_start};
    if (rel_start + length > bytes_before_payload.size()) return {};
    return bytes_before_payload.subspan(rel_start, length);
}

} // namespace

bool ParseBlockDiskHeader(const CChainParams& params, const uint32_t payload_pos, Span<const uint8_t> bytes_before_payload,
                          BlockDiskHeader& result)
{
    result = {};
    if (payload_pos < BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE || bytes_before_payload.empty() ||
        bytes_before_payload.size() > payload_pos) {
        return false;
    }

    // Extended format: magic (4) + flags (1) + stored_size (4).
    if (payload_pos >= BLOCK_SERIALIZATION_HEADER_SIZE) {
        const auto extended{BytesAtOffset(bytes_before_payload, payload_pos, payload_pos - BLOCK_SERIALIZATION_HEADER_SIZE,
                                          BLOCK_SERIALIZATION_HEADER_SIZE)};
        if (extended.size() == BLOCK_SERIALIZATION_HEADER_SIZE && MatchesMagic(params, extended.first(4))) {
            const uint8_t flags{extended[4]};
            const uint32_t stored_size{ReadLE32(extended.subspan(5, 4))};
            const bool valid_size{(flags & BLOCK_SERIALIZATION_FLAG_COMPRESSED) ?
                                      ValidCompressedStoredSize(stored_size) :
                                      ValidUncompressedStoredSize(stored_size)};
            if (ValidBlockDiskFlags(flags) && valid_size) {
                result.header_size = BLOCK_SERIALIZATION_HEADER_SIZE;
                result.flags = flags;
                result.stored_size = stored_size;
                result.legacy_format = false;
                return true;
            }
        }
    }

    // Legacy format: magic (4) + size (4).
    const auto legacy{BytesAtOffset(bytes_before_payload, payload_pos, payload_pos - BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE,
                                    BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE)};
    if (legacy.size() == BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE && MatchesMagic(params, legacy.first(4))) {
        const uint32_t stored_size{ReadLE32(legacy.subspan(4, 4))};
        if (ValidUncompressedStoredSize(stored_size)) {
            result.header_size = BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE;
            result.flags = 0;
            result.stored_size = stored_size;
            result.legacy_format = true;
            return true;
        }
    }

    return false;
}

bool ParseBlockDiskHeaderAfterMagic(const CChainParams& params, const uint32_t magic_offset,
                                    Span<const uint8_t> post_magic_bytes, BlockDiskHeader& result,
                                    uint32_t& payload_offset)
{
    result = {};
    payload_offset = 0;
    if (post_magic_bytes.size() < 4) {
        return false;
    }

    const uint8_t b0{post_magic_bytes[0]};

    if (post_magic_bytes.size() >= 5 && ValidBlockDiskFlags(b0)) {
        const uint32_t extended_size{ReadLE32(post_magic_bytes.subspan(1, 4))};
        const bool valid_size{(b0 & BLOCK_SERIALIZATION_FLAG_COMPRESSED) ?
                                  ValidCompressedStoredSize(extended_size) :
                                  ValidUncompressedStoredSize(extended_size)};
        if (valid_size) {
            result.header_size = BLOCK_SERIALIZATION_HEADER_SIZE;
            result.flags = b0;
            result.stored_size = extended_size;
            result.legacy_format = false;
            payload_offset = magic_offset + BLOCK_SERIALIZATION_HEADER_SIZE;
            return true;
        }
    }

    const uint32_t legacy_size{ReadLE32(post_magic_bytes.first(4))};
    if (ValidUncompressedStoredSize(legacy_size)) {
        result.header_size = BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE;
        result.flags = 0;
        result.stored_size = legacy_size;
        result.legacy_format = true;
        payload_offset = magic_offset + BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE;
        return true;
    }

    return false;
}

} // namespace node
// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCKFILE_FORMAT_H
#define BITCOIN_NODE_BLOCKFILE_FORMAT_H

#include <kernel/messagestartchars.h>
#include <span.h>

#include <cstdint>

class CChainParams;

namespace node {

/** Legacy per-block header: 4-byte magic + 4-byte payload size (8 bytes). */
static constexpr uint32_t BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE{std::tuple_size_v<MessageStartChars> + sizeof(unsigned int)};

/**
 * Extended per-block header for zstd-compressed storage: magic (4) + flags (1) + stored_size (4).
 * `pos.nPos` in FlatFilePos points to the first payload byte after this header.
 */
static constexpr uint32_t BLOCK_SERIALIZATION_HEADER_SIZE{std::tuple_size_v<MessageStartChars> + sizeof(uint8_t) + sizeof(unsigned int)};

/** Bit 0 of the per-block flags byte: payload is zstd-compressed. */
static constexpr uint8_t BLOCK_SERIALIZATION_FLAG_COMPRESSED{0x01};

/** Bits 1-4 of the per-block flags byte: typed dictionary bucket id (0-15). */
static constexpr uint8_t BLOCK_SERIALIZATION_FLAG_BUCKET_SHIFT{1};
static constexpr uint8_t BLOCK_SERIALIZATION_FLAG_BUCKET_MASK{0x1E};

/** Encode a typed block bucket id into the per-block flags byte. */
constexpr uint8_t BlockBucketFlags(uint8_t bucket_id) { return static_cast<uint8_t>(bucket_id << BLOCK_SERIALIZATION_FLAG_BUCKET_SHIFT); }

/** Extract the typed block bucket id from the per-block flags byte. */
constexpr uint8_t BlockBucketFromFlags(uint8_t flags) { return static_cast<uint8_t>((flags & BLOCK_SERIALIZATION_FLAG_BUCKET_MASK) >> BLOCK_SERIALIZATION_FLAG_BUCKET_SHIFT); }

/** Parsed per-block disk header preceding the payload in blk*.dat files. */
struct BlockDiskHeader
{
    uint32_t header_size{0};
    uint8_t flags{0};
    uint32_t stored_size{0};
    bool legacy_format{false};
};

/** Return true when @p flags uses only known compressed and bucket-id bits. */
constexpr bool ValidBlockDiskFlags(uint8_t flags)
{
    return (flags & ~(BLOCK_SERIALIZATION_FLAG_COMPRESSED | BLOCK_SERIALIZATION_FLAG_BUCKET_MASK)) == 0;
}

/** Returns true when @p header indicates a zstd-compressed on-disk payload. */
constexpr bool BlockDiskPayloadIsCompressed(const BlockDiskHeader& header)
{
    return (header.flags & BLOCK_SERIALIZATION_FLAG_COMPRESSED) != 0;
}

/** Returns false when @p header is compressed but @p allow_decompress is disabled. */
constexpr bool CompressedBlockReadAllowed(const BlockDiskHeader& header, bool allow_decompress)
{
    return !BlockDiskPayloadIsCompressed(header) || allow_decompress;
}

/**
 * Parse the per-block header ending immediately before @p payload_pos.
 * @param bytes_before_payload Bytes read from the file ending at offset @p payload_pos (exclusive).
 */
bool ParseBlockDiskHeader(const CChainParams& params, uint32_t payload_pos, Span<const uint8_t> bytes_before_payload,
                          BlockDiskHeader& result);

/**
 * Parse a header when the 4-byte message-start magic has already been read at @p magic_offset.
 * @param post_magic_bytes Up to five bytes immediately following the magic.
 * @param payload_offset Set to the file offset of the first payload byte on success.
 */
bool ParseBlockDiskHeaderAfterMagic(const CChainParams& params, uint32_t magic_offset,
                                    Span<const uint8_t> post_magic_bytes, BlockDiskHeader& result,
                                    uint32_t& payload_offset);

} // namespace node

#endif // BITCOIN_NODE_BLOCKFILE_FORMAT_H
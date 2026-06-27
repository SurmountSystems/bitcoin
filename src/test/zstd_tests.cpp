// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <compress/zstd.h>
#include <index/disktxpos.h>
#include <logging.h>
#include <node/blockfile_format.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <primitives/block.h>
#include <script/script.h>
#include <streams.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

#include <array>
#include <fstream>
#include <vector>

using node::BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE;
using node::BLOCK_SERIALIZATION_FLAG_COMPRESSED;
using node::BLOCK_SERIALIZATION_HEADER_SIZE;
using node::BlockManager;
using node::KernelNotifications;
using node::ParseBlockDiskHeader;
using node::ParseBlockDiskHeaderAfterMagic;
using node::ValidBlockDiskFlags;

namespace {

static std::vector<std::string> ReadDebugLogLines()
{
    std::vector<std::string> lines;
    std::ifstream ifs{LogInstance().m_file_path};
    for (std::string line; std::getline(ifs, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

struct LogSetup : public BasicTestingSetup {
    fs::path prev_log_path;
    fs::path tmp_log_path;
    bool prev_reopen_file;
    bool prev_print_to_file;

    LogSetup()
        : prev_log_path{LogInstance().m_file_path},
          tmp_log_path{m_args.GetDataDirBase() / "tmp_debug.log"},
          prev_reopen_file{LogInstance().m_reopen_file},
          prev_print_to_file{LogInstance().m_print_to_file}
    {
        LogInstance().m_file_path = tmp_log_path;
        LogInstance().m_reopen_file = true;
        LogInstance().m_print_to_file = true;
        LogInstance().m_log_timestamps = false;
        LogInstance().m_log_threadnames = false;
        LogInstance().m_log_sourcelocations = false;
    }

    ~LogSetup()
    {
        LogInstance().m_file_path = prev_log_path;
        LogPrintf("Sentinel log to reopen log file\n");
        LogInstance().m_print_to_file = prev_print_to_file;
        LogInstance().m_reopen_file = prev_reopen_file;
    }
};

CBlock MakeRepetitiveBlock()
{
    CBlock block;
    block.nVersion = 4;
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.resize(1);
    tx.vout.resize(1);
    tx.vout[0].nValue = 50 * COIN;
    tx.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<uint8_t>(1024, 0x42);
    for (int i = 0; i < 32; ++i) {
        block.vtx.push_back(MakeTransactionRef(tx));
    }
    return block;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(zstd_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(zstd_roundtrip)
{
    const auto dict{compress::LoadDictionaryFile(compress::DefaultBlockDictionaryPath())};
    BOOST_REQUIRE(dict);
    compress::BlockZstd zstd{*dict};
    BOOST_REQUIRE(zstd);

    std::vector<uint8_t> input(512);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<uint8_t>(i % 251);
    }

    std::vector<uint8_t> compressed;
    BOOST_CHECK(zstd.Compress(input, compressed, 10));
    BOOST_CHECK(!compressed.empty());
    BOOST_CHECK(compressed.size() < input.size());

    std::vector<uint8_t> output;
    BOOST_CHECK(zstd.Decompress(compressed, output, input.size() * 2));
    BOOST_CHECK_EQUAL_COLLECTIONS(input.begin(), input.end(), output.begin(), output.end());
}

BOOST_AUTO_TEST_CASE(zstd_corrupt_payload_decompress_fails)
{
    const auto dict{compress::LoadDictionaryFile(compress::DefaultBlockDictionaryPath())};
    BOOST_REQUIRE(dict);
    compress::BlockZstd zstd{*dict};
    BOOST_REQUIRE(zstd);

    const std::vector<uint8_t> corrupt{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x01, 0x02};
    std::vector<uint8_t> output;
    BOOST_CHECK(!zstd.Decompress(corrupt, output, 512));
}

BOOST_AUTO_TEST_CASE(blockfile_format_legacy_and_extended)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const auto& magic{params->MessageStart()};

    std::vector<uint8_t> prefix(8);
    prefix[0] = magic[0];
    prefix[1] = magic[1];
    prefix[2] = magic[2];
    prefix[3] = magic[3];
    prefix[4] = 0x00;
    prefix[5] = 0x01;
    prefix[6] = 0x00;
    prefix[7] = 0x00; // legacy size 256 (little-endian)

    node::BlockDiskHeader header;
    BOOST_CHECK(ParseBlockDiskHeader(*params, 8, prefix, header));
    BOOST_CHECK(header.legacy_format);
    BOOST_CHECK_EQUAL(header.header_size, BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE);
    BOOST_CHECK_EQUAL(header.stored_size, 256);

    std::array<uint8_t, 5> post_magic{0x01, 0x80, 0x00, 0x00, 0x00}; // flags=compressed, size=128 LE
    uint32_t payload_offset{0};
    BOOST_CHECK(ParseBlockDiskHeaderAfterMagic(*params, 0, post_magic, header, payload_offset));
    BOOST_CHECK(!header.legacy_format);
    BOOST_CHECK(header.flags & BLOCK_SERIALIZATION_FLAG_COMPRESSED);
    BOOST_CHECK_EQUAL(header.header_size, BLOCK_SERIALIZATION_HEADER_SIZE);
    BOOST_CHECK_EQUAL(payload_offset, BLOCK_SERIALIZATION_HEADER_SIZE);

    BOOST_CHECK(!ValidBlockDiskFlags(0x02));
    BOOST_CHECK(ValidBlockDiskFlags(0x00));
    BOOST_CHECK(ValidBlockDiskFlags(0x01));

    std::vector<uint8_t> extended_prefix(BLOCK_SERIALIZATION_HEADER_SIZE);
    extended_prefix[0] = magic[0];
    extended_prefix[1] = magic[1];
    extended_prefix[2] = magic[2];
    extended_prefix[3] = magic[3];
    extended_prefix[4] = 0x02; // reserved flag bit
    extended_prefix[5] = 0x50;
    extended_prefix[6] = 0x00;
    extended_prefix[7] = 0x00;
    extended_prefix[8] = 0x00; // stored size 80 LE
    BOOST_CHECK(!ParseBlockDiskHeader(*params, BLOCK_SERIALIZATION_HEADER_SIZE, extended_prefix, header));
}

BOOST_AUTO_TEST_CASE(blockmanager_legacy_write_format)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
        .block_zstd = false,
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};

    CBlock block;
    block.nVersion = 3;
    const FlatFilePos pos{blockman.WriteBlock(block, 1)};
    BOOST_CHECK_EQUAL(pos.nPos, BLOCK_LEGACY_SERIALIZATION_HEADER_SIZE);

    std::vector<uint8_t> raw;
    BOOST_CHECK(blockman.ReadRawBlock(raw, pos));
    CBlock read_block;
    BOOST_CHECK_NO_THROW(SpanReader{raw} >> TX_WITH_WITNESS(read_block));
    BOOST_CHECK_EQUAL(read_block.nVersion, block.nVersion);
}

BOOST_AUTO_TEST_CASE(blockmanager_write_read_roundtrip)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
        .block_zstd = true,
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};

    const CBlock block{MakeRepetitiveBlock()};

    const auto plaintext_size{static_cast<unsigned int>(GetSerializeSize(TX_WITH_WITNESS(block)))};
    const FlatFilePos pos{blockman.WriteBlock(block, 1)};
    BOOST_CHECK_EQUAL(pos.nPos, BLOCK_SERIALIZATION_HEADER_SIZE);

    std::vector<uint8_t> raw;
    BOOST_CHECK(blockman.ReadRawBlock(raw, pos));
    BOOST_CHECK_EQUAL(raw.size(), plaintext_size);

    // Verify on-disk payload is compressed (smaller than plaintext).
    std::vector<uint8_t> stored;
    {
        const uint32_t probe_size{BLOCK_SERIALIZATION_HEADER_SIZE};
        AutoFile file{blockman.OpenBlockFile({pos.nFile, pos.nPos - probe_size}, /*fReadOnly=*/true)};
        BOOST_REQUIRE(!file.IsNull());
        std::array<uint8_t, BLOCK_SERIALIZATION_HEADER_SIZE> header_bytes{};
        file.read(MakeWritableByteSpan(header_bytes));
        node::BlockDiskHeader disk_header;
        BOOST_REQUIRE(ParseBlockDiskHeader(*params, pos.nPos, header_bytes, disk_header));
        const bool compressed{(disk_header.flags & BLOCK_SERIALIZATION_FLAG_COMPRESSED) != 0};
        stored.resize(disk_header.stored_size);
        file.read(MakeWritableByteSpan(stored));
        if (compressed) {
            BOOST_CHECK_LT(stored.size(), plaintext_size);
        } else {
            BOOST_CHECK_EQUAL(stored.size(), plaintext_size);
        }
    }

    CBlock read_block;
    BOOST_CHECK_NO_THROW(SpanReader{raw} >> TX_WITH_WITNESS(read_block));
    BOOST_CHECK_EQUAL(read_block.nVersion, block.nVersion);
}

BOOST_AUTO_TEST_CASE(blockmanager_decompress_disabled)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};

    BlockManager::Options write_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
        .block_zstd = true,
    };

    FlatFilePos pos;
    {
        BlockManager writer{*Assert(m_node.shutdown_signal), write_opts};
        CBlock block;
        block.nVersion = 5;
        CMutableTransaction tx;
        tx.version = 2;
        block.vtx.push_back(MakeTransactionRef(tx));
        pos = writer.WriteBlock(block, 1);
    }

    BlockManager::Options read_opts{write_opts};
    read_opts.block_zstd = false;
    read_opts.block_zstd_decompress = false;
    BlockManager reader{*Assert(m_node.shutdown_signal), read_opts};

    std::vector<uint8_t> raw;
    BOOST_CHECK(!reader.ReadRawBlock(raw, pos));
}

BOOST_AUTO_TEST_CASE(txindex_findtx_compressed_block)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
        .block_zstd = true,
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};

    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.resize(1);
    tx.vout.resize(1);
    tx.vout[0].nValue = 50 * COIN;
    CBlock block;
    block.nVersion = 6;
    block.vtx.push_back(MakeTransactionRef(tx));

    const FlatFilePos pos{blockman.WriteBlock(block, 1)};
    const CDiskTxPos tx_pos{pos, GetSizeOfCompactSize(block.vtx.size())};

    // Emulate TxIndex::FindTx deserialization using ReadRawBlock + offsets.
    std::vector<uint8_t> block_data;
    BOOST_REQUIRE(blockman.ReadRawBlock(block_data, pos));
    CTransactionRef tx_disk;
    CBlockHeader header;
    SpanReader reader{block_data};
    reader >> header;
    reader.ignore(tx_pos.nTxOffset);
    reader >> TX_WITH_WITNESS(tx_disk);
    BOOST_CHECK_EQUAL(tx_disk->GetHash(), tx.GetHash());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_missing_dictionary_startup_warnings, LogSetup)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{Assert(m_node.shutdown_request), m_node.exit_status, *Assert(m_node.warnings)};
    const fs::path invalid_dict{m_args.GetDataDirBase() / "missing-blk.dict"};

    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
        .block_zstd = true,
        .block_zstd_decompress = true,
        .block_zstd_dict = invalid_dict,
    };
    BlockManager blockman{*Assert(m_node.shutdown_signal), blockman_opts};

    const std::vector<std::string> log_lines{ReadDebugLogLines()};
    bool saw_compress_warning{false};
    bool saw_decompress_warning{false};
    for (const std::string& line : log_lines) {
        if (line.find("Block zstd compression is enabled (-blockzstd=1) but no dictionary is loaded") != std::string::npos) {
            saw_compress_warning = true;
        }
        if (line.find("Block zstd decompression is enabled (-blockzstddecompress=1) but no dictionary is loaded") != std::string::npos) {
            saw_decompress_warning = true;
        }
    }
    BOOST_CHECK(saw_compress_warning);
    BOOST_CHECK(saw_decompress_warning);
}

BOOST_AUTO_TEST_SUITE_END()
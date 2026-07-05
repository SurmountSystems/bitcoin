// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <kernel/chainparams.h>
#include <dbwrapper.h>
#include <random.h>
#include <node/blockfile_format.h>
using node::BlockDiskPayloadIsCompressed;
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <primitives/block.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <util/benchstats.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

#include <array>
#include <cstring>
#include <thread>
#include <vector>

using node::BLOCK_SERIALIZATION_HEADER_SIZE;
using node::BlockManager;
using node::KernelNotifications;
using node::ParseBlockDiskHeader;

namespace {
CBlock MakeLargeRepetitiveBlock()
{
    CBlock block;
    block.nVersion = 4;
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.resize(1);
    tx.vout.resize(1);
    tx.vout[0].nValue = 50 * COIN;
    std::vector<uint8_t> payload(512 * 1024);
    for (size_t off = 0; off < payload.size(); off += 32) {
        const size_t chunk{std::min<size_t>(32, payload.size() - off)};
        GetRandBytes({payload.data() + off, chunk});
    }
    tx.vout[0].scriptPubKey = CScript() << OP_RETURN << payload;
    block.vtx.push_back(MakeTransactionRef(tx));
    return block;
}

CBlock MakeSmallBlock()
{
    CBlock block;
    block.nVersion = 3;
    return block;
}

struct BlockmanHarness
{
    BlockManager blockman;

    explicit BlockmanHarness(BasicTestingSetup& setup)
        : blockman{*Assert(setup.m_node.shutdown_signal), MakeOpts(setup)}
    {
    }

private:
    static BlockManager::Options MakeOpts(BasicTestingSetup& setup)
    {
        const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
        KernelNotifications notifications{Assert(setup.m_node.shutdown_request), setup.m_node.exit_status, *Assert(setup.m_node.warnings)};
        return BlockManager::Options{
            .chainparams = *params,
            .blocks_dir = setup.m_args.GetBlocksDirPath(),
            .notifications = notifications,
            .block_tree_db_params = DBParams{
                .path = setup.m_args.GetDataDirNet() / "blocks" / "index",
                .cache_bytes = 0,
            },
            .block_zstd = true,
            .block_zstd_decompress = true,
            .block_decompress_workers = 4,
        };
    }
};

bool LoadStored(BlockManager& blockman,
                const FlatFilePos& pos,
                const CChainParams& params,
                node::BlockDiskHeader& header,
                std::vector<uint8_t>& stored)
{
    const uint32_t probe_size{BLOCK_SERIALIZATION_HEADER_SIZE};
    AutoFile file{blockman.OpenBlockFile({pos.nFile, pos.nPos - probe_size}, /*fReadOnly=*/true)};
    if (file.IsNull()) return false;
    std::array<uint8_t, BLOCK_SERIALIZATION_HEADER_SIZE> header_bytes{};
    file.read(MakeWritableByteSpan(header_bytes));
    if (!ParseBlockDiskHeader(params, pos.nPos, header_bytes, header)) return false;
    stored.resize(header.stored_size);
    file.read(MakeWritableByteSpan(stored));
    return true;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(block_decompress_parallel_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(decompress_serial_vs_parallel_memcmp)
{
    BlockmanHarness harness{*this};
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};

    const CBlock block{MakeLargeRepetitiveBlock()};
    const FlatFilePos pos{harness.blockman.WriteBlock(block, 1)};
    BOOST_REQUIRE(!pos.IsNull());

    node::BlockDiskHeader header;
    std::vector<uint8_t> stored;
    BOOST_REQUIRE(LoadStored(harness.blockman, pos, *params, header, stored));
    BOOST_REQUIRE_GE(stored.size(), kernel::BLOCK_DECOMPRESS_PARALLEL_MIN_SIZE);

    std::vector<uint8_t> serial;
    std::vector<uint8_t> parallel;
    for (int i = 0; i < 100; ++i) {
        harness.blockman.ConfigureDecompressPool(0);
        BOOST_REQUIRE(harness.blockman.ReadRawBlockFromStored(serial, header, stored, /*allow_parallel_decompress=*/true));
        harness.blockman.ConfigureDecompressPool(4);
        harness.blockman.SetIbdParallelReadsAllowed(true);
        BOOST_REQUIRE(harness.blockman.ReadRawBlockFromStored(parallel, header, stored, /*allow_parallel_decompress=*/true));
        BOOST_REQUIRE_EQUAL(serial.size(), parallel.size());
        BOOST_CHECK_EQUAL(std::memcmp(serial.data(), parallel.data(), serial.size()), 0);
    }
}

BOOST_AUTO_TEST_CASE(decompress_gating_ibd_and_payload_size)
{
    BlockmanHarness harness{*this};
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};

    const CBlock large{MakeLargeRepetitiveBlock()};
    const FlatFilePos large_pos{harness.blockman.WriteBlock(large, 1)};
    node::BlockDiskHeader large_header;
    std::vector<uint8_t> large_stored;
    BOOST_REQUIRE(LoadStored(harness.blockman, large_pos, *params, large_header, large_stored));

    const CBlock small{MakeSmallBlock()};
    const FlatFilePos small_pos{harness.blockman.WriteBlock(small, 2)};
    node::BlockDiskHeader small_header;
    std::vector<uint8_t> small_stored;
    BOOST_REQUIRE(LoadStored(harness.blockman, small_pos, *params, small_header, small_stored));
    BOOST_REQUIRE_LT(small_stored.size(), kernel::BLOCK_DECOMPRESS_PARALLEL_MIN_SIZE);

    util::g_benchstats_enabled.store(true);
    util::g_benchstats.block_decompress_jobs.store(0);

    harness.blockman.ConfigureDecompressPool(4);
    harness.blockman.SetIbdParallelReadsAllowed(false);
    std::vector<uint8_t> out;
    BOOST_REQUIRE(harness.blockman.ReadRawBlockFromStored(out, large_header, large_stored, /*allow_parallel_decompress=*/true));
    BOOST_CHECK_EQUAL(util::g_benchstats.block_decompress_jobs.load(), 0u);

    util::g_benchstats.block_decompress_jobs.store(0);
    harness.blockman.SetIbdParallelReadsAllowed(true);
    BOOST_REQUIRE(harness.blockman.ReadRawBlockFromStored(out, small_header, small_stored, /*allow_parallel_decompress=*/true));
    BOOST_CHECK_EQUAL(util::g_benchstats.block_decompress_jobs.load(), 0u);

    util::g_benchstats.block_decompress_jobs.store(0);
    std::vector<uint8_t> serial_ref;
    harness.blockman.ConfigureDecompressPool(0);
    BOOST_REQUIRE(harness.blockman.ReadRawBlockFromStored(serial_ref, large_header, large_stored, /*allow_parallel_decompress=*/true));
    harness.blockman.ConfigureDecompressPool(4);
    harness.blockman.SetIbdParallelReadsAllowed(true);
    BOOST_REQUIRE(harness.blockman.ReadRawBlockFromStored(out, large_header, large_stored, /*allow_parallel_decompress=*/true));
    BOOST_REQUIRE_EQUAL(out.size(), serial_ref.size());
    BOOST_CHECK_EQUAL(std::memcmp(out.data(), serial_ref.data(), out.size()), 0);
    if (BlockDiskPayloadIsCompressed(large_header)) {
        BOOST_CHECK_GT(util::g_benchstats.block_decompress_jobs.load(), 0u);
    }
}

BOOST_AUTO_TEST_CASE(decompress_pool_queue_overflow_still_succeeds)
{
    BlockmanHarness harness{*this};
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};

    const CBlock block{MakeLargeRepetitiveBlock()};
    const FlatFilePos pos{harness.blockman.WriteBlock(block, 1)};
    node::BlockDiskHeader header;
    std::vector<uint8_t> stored;
    BOOST_REQUIRE(LoadStored(harness.blockman, pos, *params, header, stored));

    std::vector<uint8_t> reference;
    harness.blockman.ConfigureDecompressPool(0);
    BOOST_REQUIRE(harness.blockman.ReadRawBlockFromStored(reference, header, stored, /*allow_parallel_decompress=*/true));

    harness.blockman.ConfigureDecompressPool(4);
    harness.blockman.SetIbdParallelReadsAllowed(true);
    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            std::vector<uint8_t> out;
            if (harness.blockman.ReadRawBlockFromStored(out, header, stored, /*allow_parallel_decompress=*/true)
                && out.size() == reference.size()
                && std::memcmp(out.data(), reference.data(), out.size()) == 0) {
                successes.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    BOOST_CHECK_EQUAL(successes.load(), 8);
}

BOOST_AUTO_TEST_SUITE_END()
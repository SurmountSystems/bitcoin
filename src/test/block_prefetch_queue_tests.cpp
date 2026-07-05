// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <dbwrapper.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <util/signalinterrupt.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

#include <chrono>
#include <cstring>
#include <thread>

using node::BlockManager;
using node::BlockPrefetchQueue;
using node::BlockReadLoc;
using node::KernelNotifications;

namespace {
BlockReadLoc MakeValidLoc(const FlatFilePos& pos, const uint256& hash)
{
    BlockReadLoc loc;
    loc.pos = pos;
    loc.hash = hash;
    loc.have_data = true;
    return loc;
}

BlockManager MakeBlockman(BasicTestingSetup& setup)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    KernelNotifications notifications{Assert(setup.m_node.shutdown_request), setup.m_node.exit_status, *Assert(setup.m_node.warnings)};
    BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = setup.m_args.GetBlocksDirPath(),
        .notifications = notifications,
        .block_tree_db_params = DBParams{
            .path = setup.m_args.GetDataDirNet() / "blocks" / "index",
            .cache_bytes = 0,
        },
        .block_zstd = true,
    };
    return BlockManager{*Assert(setup.m_node.shutdown_signal), blockman_opts};
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(block_prefetch_queue_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(invalidate_drops_stale_async_result)
{
    BlockManager blockman{MakeBlockman(*this)};

    CBlock block;
    block.nVersion = 3;
    const FlatFilePos pos{blockman.WriteBlock(block, 1)};
    BOOST_REQUIRE(!pos.IsNull());
    const uint256 hash{block.GetHash()};

    BlockPrefetchQueue queue;
    const BlockReadLoc loc{MakeValidLoc(pos, hash)};
    blockman.SetIbdParallelReadsAllowed(true);
    queue.Enqueue(loc, blockman, *Assert(m_node.shutdown_signal));

    queue.Invalidate();
    queue.WaitForIdle();

    BOOST_CHECK(!queue.TakeIfReady(loc).has_value());
}

BOOST_AUTO_TEST_CASE(enqueue_returns_without_waiting_for_worker)
{
    BlockManager blockman{MakeBlockman(*this)};

    CBlock block;
    block.nVersion = 3;
    const FlatFilePos pos{blockman.WriteBlock(block, 1)};
    const BlockReadLoc loc{MakeValidLoc(pos, block.GetHash())};

    BlockPrefetchQueue queue;
    blockman.SetIbdParallelReadsAllowed(true);
    const auto start{std::chrono::steady_clock::now()};
    queue.Enqueue(loc, blockman, *Assert(m_node.shutdown_signal));
    const auto elapsed{std::chrono::steady_clock::now() - start};
    BOOST_CHECK(elapsed < std::chrono::milliseconds{200});

    queue.Invalidate();
    queue.WaitForIdle();
}

BOOST_AUTO_TEST_CASE(take_if_ready_matches_loc_only)
{
    BlockManager blockman{MakeBlockman(*this)};

    CBlock block;
    block.nVersion = 3;
    const FlatFilePos pos{blockman.WriteBlock(block, 1)};
    const BlockReadLoc loc{MakeValidLoc(pos, block.GetHash())};
    BlockReadLoc other{loc};
    other.hash = uint256::ONE;

    BlockPrefetchQueue queue;
    blockman.SetIbdParallelReadsAllowed(true);
    queue.Enqueue(loc, blockman, *Assert(m_node.shutdown_signal));

    for (int i = 0; i < 200 && !queue.TakeIfReady(loc).has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    BOOST_CHECK(!queue.TakeIfReady(other).has_value());

    queue.WaitForIdle();
}

BOOST_AUTO_TEST_CASE(interrupt_during_read_prevents_ready_payload)
{
    BlockManager blockman{MakeBlockman(*this)};

    CBlock block;
    block.nVersion = 3;
    const FlatFilePos pos{blockman.WriteBlock(block, 1)};
    const BlockReadLoc loc{MakeValidLoc(pos, block.GetHash())};

    util::SignalInterrupt interrupt;
    BlockPrefetchQueue queue;
    blockman.SetIbdParallelReadsAllowed(true);
    interrupt();
    queue.Enqueue(loc, blockman, interrupt);
    queue.WaitForIdle();

    BOOST_CHECK(!queue.TakeIfReady(loc).has_value());
}

BOOST_AUTO_TEST_CASE(replacement_prefers_latest_loc)
{
    BlockManager blockman{MakeBlockman(*this)};

    CBlock block_a;
    block_a.nVersion = 3;
    const FlatFilePos pos_a{blockman.WriteBlock(block_a, 1)};
    const BlockReadLoc loc_a{MakeValidLoc(pos_a, block_a.GetHash())};

    CBlock block_b;
    block_b.nVersion = 4;
    const FlatFilePos pos_b{blockman.WriteBlock(block_b, 2)};
    const BlockReadLoc loc_b{MakeValidLoc(pos_b, block_b.GetHash())};

    BlockPrefetchQueue queue;
    blockman.SetIbdParallelReadsAllowed(true);
    queue.Enqueue(loc_a, blockman, *Assert(m_node.shutdown_signal));
    queue.Enqueue(loc_b, blockman, *Assert(m_node.shutdown_signal));

    std::optional<std::vector<uint8_t>> payload_b;
    for (int i = 0; i < 200 && !(payload_b = queue.TakeIfReady(loc_b)).has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    BOOST_REQUIRE(payload_b.has_value());
    BOOST_CHECK(!queue.TakeIfReady(loc_a).has_value());

    std::vector<uint8_t> sync;
    BOOST_REQUIRE(blockman.ReadBlockFromPayload(sync, loc_b, /*lowprio=*/false));
    BOOST_REQUIRE_EQUAL(payload_b->size(), sync.size());
    BOOST_CHECK_EQUAL(std::memcmp(payload_b->data(), sync.data(), sync.size()), 0);

    queue.WaitForIdle();
}

BOOST_AUTO_TEST_CASE(prefetch_payload_matches_sync_read)
{
    BlockManager blockman{MakeBlockman(*this)};

    CBlock block;
    block.nVersion = 3;
    const FlatFilePos pos{blockman.WriteBlock(block, 1)};
    const BlockReadLoc loc{MakeValidLoc(pos, block.GetHash())};

    BlockPrefetchQueue queue;
    blockman.SetIbdParallelReadsAllowed(true);
    queue.Enqueue(loc, blockman, *Assert(m_node.shutdown_signal));

    std::optional<std::vector<uint8_t>> prefetched;
    for (int i = 0; i < 200 && !(prefetched = queue.TakeIfReady(loc)).has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    BOOST_REQUIRE(prefetched.has_value());

    std::vector<uint8_t> sync;
    BOOST_REQUIRE(blockman.ReadBlockFromPayload(sync, loc, /*lowprio=*/false));
    BOOST_REQUIRE_EQUAL(prefetched->size(), sync.size());
    BOOST_CHECK_EQUAL(std::memcmp(prefetched->data(), sync.data(), sync.size()), 0);

    queue.WaitForIdle();
}

BOOST_AUTO_TEST_SUITE_END()
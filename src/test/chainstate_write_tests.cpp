// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

// Taken from validation.cpp
static constexpr auto DATABASE_WRITE_INTERVAL_MIN{50min};
static constexpr auto DATABASE_WRITE_INTERVAL_MAX{70min};

BOOST_AUTO_TEST_SUITE(chainstate_write_tests)

BOOST_FIXTURE_TEST_CASE(chainstate_write_interval, TestingSetup)
{
    struct TestSubscriber final : CValidationInterface {
        bool m_did_flush{false};
        void ChainStateFlushed(ChainstateRole, const CBlockLocator&) override
        {
            m_did_flush = true;
        }
    };

    const auto sub{std::make_shared<TestSubscriber>()};
    m_node.validation_signals->RegisterSharedValidationInterface(sub);
    auto& chainstate{Assert(m_node.chainman)->ActiveChainstate()};
    BlockValidationState state_dummy{};

    // The first periodic flush sets m_next_write and does not flush
    chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(!sub->m_did_flush);

    // The periodic flush interval is between 50 and 70 minutes (inclusive)
    SetMockTime(GetTime<std::chrono::minutes>() + DATABASE_WRITE_INTERVAL_MIN - 1min);
    chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(!sub->m_did_flush);

    SetMockTime(GetTime<std::chrono::minutes>() + DATABASE_WRITE_INTERVAL_MAX);
    chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(sub->m_did_flush);
}

// Test that we do PERIODIC flushes inside ActivateBestChain.
// This is necessary for reindex-chainstate to be able to periodically flush
// before reaching chain tip.
BOOST_FIXTURE_TEST_CASE(write_during_multiblock_activation, TestChain100Setup)
{
    struct TestSubscriber final : CValidationInterface
    {
        const CBlockIndex* m_tip{nullptr};
        const CBlockIndex* m_flushed_at_block{nullptr};
        void ChainStateFlushed(ChainstateRole, const CBlockLocator&) override
        {
            m_flushed_at_block = m_tip;
        }
        void UpdatedBlockTip(const CBlockIndex* block_index, const CBlockIndex*, bool) override {
            m_tip = block_index;
        }
    };

    auto& chainstate{Assert(m_node.chainman)->ActiveChainstate()};
    BlockValidationState state_dummy{};

    // Pop two blocks from the tip
    const CBlockIndex* tip{chainstate.m_chain.Tip()};
    CBlockIndex* second_from_tip{tip->pprev};

    {
        LOCK2(m_node.chainman->GetMutex(), chainstate.MempoolMutex());
        chainstate.DisconnectTip(state_dummy, nullptr);
        chainstate.DisconnectTip(state_dummy, nullptr);
    }

    BOOST_CHECK_EQUAL(second_from_tip->pprev, chainstate.m_chain.Tip());

    // Set m_next_write to current time
    chainstate.FlushStateToDisk(state_dummy, FlushStateMode::ALWAYS);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    // The periodic flush interval is between 50 and 70 minutes (inclusive)
    // The next call to a PERIODIC write will flush
    SetMockTime(GetMockTime() + DATABASE_WRITE_INTERVAL_MAX);

    const auto sub{std::make_shared<TestSubscriber>()};
    m_node.validation_signals->RegisterSharedValidationInterface(sub);

    // ActivateBestChain back to tip
    chainstate.ActivateBestChain(state_dummy, nullptr);
    BOOST_CHECK_EQUAL(tip, chainstate.m_chain.Tip());
    // Check that we flushed inside ActivateBestChain while we were at the
    // second block from tip, since FlushStateToDisk is called with PERIODIC
    // inside the outer loop.
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK_EQUAL(sub->m_flushed_at_block, second_from_tip);
}

//! During IBD, periodic flush uses a lower threshold than synced operation.
BOOST_FIXTURE_TEST_CASE(ibd_flush_threshold, TestChain100Setup)
{
    auto& chainman = *Assert(m_node.chainman);
    auto& chainstate = chainman.ActiveChainstate();

    // 100 MiB coinstip budget; IBD threshold min(4 GiB default, 15 MiB) = 15 MiB.
    // Synced threshold max(90 MiB, ~100 MiB - 10 MiB) = 90 MiB.
    constexpr size_t MAX_COINS_CACHE_BYTES = 100 * 1024 * 1024;

    LOCK(::cs_main);
    auto& view = chainstate.CoinsTip();

    auto add_coins_until = [&](CoinsCacheSizeState target) {
        while (chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes=*/0) < target) {
            AddTestCoin(m_rng, view);
        }
    };

    chainman.m_cached_finished_ibd.store(false, std::memory_order_relaxed);
    BOOST_CHECK(chainman.IsInitialBlockDownload());

    add_coins_until(CoinsCacheSizeState::LARGE);
    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes=*/0),
        CoinsCacheSizeState::LARGE);
    const int64_t ibd_usage_at_large = view.DynamicMemoryUsage();
    BOOST_CHECK_LT(ibd_usage_at_large, static_cast<int64_t>(MAX_COINS_CACHE_BYTES * 9 / 10));

    static_cast<TestChainstateManager&>(chainman).JumpOutOfIbd();
    BOOST_CHECK(!chainman.IsInitialBlockDownload());
    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes=*/0),
        CoinsCacheSizeState::OK);

    add_coins_until(CoinsCacheSizeState::LARGE);
    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes=*/0),
        CoinsCacheSizeState::LARGE);
    const int64_t synced_usage_at_large = view.DynamicMemoryUsage();
    BOOST_CHECK_GT(synced_usage_at_large, ibd_usage_at_large);
}

//! -flushutxo-ibd-mib overrides the IBD absolute flush cap.
BOOST_FIXTURE_TEST_CASE(ibd_flush_threshold_custom_mib, TestChain100Setup)
{
    auto& chainman = *Assert(m_node.chainman);
    auto& chainstate = chainman.ActiveChainstate();
    const_cast<ChainstateManager::Options&>(chainman.m_options).flushutxo_ibd_mib = 8;

    constexpr size_t MAX_COINS_CACHE_BYTES = 100 * 1024 * 1024;

    LOCK(::cs_main);
    auto& view = chainstate.CoinsTip();

    chainman.m_cached_finished_ibd.store(false, std::memory_order_relaxed);
    BOOST_CHECK(chainman.IsInitialBlockDownload());

    while (chainstate.GetCoinsCacheSizeState(MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes=*/0) !=
           CoinsCacheSizeState::LARGE) {
        AddTestCoin(m_rng, view);
    }

    const int64_t usage_at_large = view.DynamicMemoryUsage();
    // min(8 MiB, 15% of 100 MiB) = 8 MiB
    BOOST_CHECK_GE(usage_at_large, 8 * 1024 * 1024);
    BOOST_CHECK_LT(usage_at_large, 15 * 1024 * 1024);
}

//! IBD LARGE threshold triggers FlushStateToDisk(PERIODIC) via fCacheLarge (end-to-end).
BOOST_FIXTURE_TEST_CASE(ibd_periodic_flush_at_large_threshold, TestChain100Setup)
{
    struct TestSubscriber final : CValidationInterface {
        bool m_did_flush{false};
        void ChainStateFlushed(ChainstateRole, const CBlockLocator&) override { m_did_flush = true; }
    };

    auto& chainman = *Assert(m_node.chainman);
    auto& chainstate = chainman.ActiveChainstate();
    constexpr size_t MAX_COINS_CACHE_BYTES = 100 * 1024 * 1024;

    LOCK(::cs_main);
    BOOST_REQUIRE(chainstate.ResizeCoinsCaches(MAX_COINS_CACHE_BYTES, chainstate.m_coinsdb_cache_size_bytes));
    auto& view = chainstate.CoinsTip();
    chainman.m_cached_finished_ibd.store(false, std::memory_order_relaxed);

    // Use production GetCoinsCacheSizeState() (includes mempool slack) — same path as FlushStateToDisk.
    while (chainstate.GetCoinsCacheSizeState() < CoinsCacheSizeState::LARGE) {
        AddTestCoin(m_rng, view);
    }
    BOOST_REQUIRE_EQUAL(chainstate.GetCoinsCacheSizeState(), CoinsCacheSizeState::LARGE);

    const auto sub{std::make_shared<TestSubscriber>()};
    m_node.validation_signals->RegisterSharedValidationInterface(sub);
    BlockValidationState state_dummy{};
    chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(sub->m_did_flush);

    sub->m_did_flush = false;
    static_cast<TestChainstateManager&>(chainman).JumpOutOfIbd();
    BOOST_CHECK_EQUAL(chainstate.GetCoinsCacheSizeState(), CoinsCacheSizeState::OK);
    chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(!sub->m_did_flush);
}

BOOST_AUTO_TEST_SUITE_END()

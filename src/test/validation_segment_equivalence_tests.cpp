// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <dbwrapper.h>
#include <node/blockstorage.h>
#include <random.h>
#include <script/script.h>
#include <sync.h>
#include <txdb.h>
#include <util/benchstats.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>

namespace {
class CCoinsViewCacheProbe : public CCoinsViewCache
{
public:
    explicit CCoinsViewCacheProbe(CCoinsView* base) : CCoinsViewCache(base) {}

    bool EntryIsDirty(const COutPoint& outpoint) const
    {
        const auto it{cacheCoins.find(outpoint)};
        return it != cacheCoins.end() && it->second.IsDirty();
    }

    bool EntryIsFresh(const COutPoint& outpoint) const
    {
        const auto it{cacheCoins.find(outpoint)};
        return it != cacheCoins.end() && it->second.IsFresh();
    }
};

void ZeroBenchStats()
{
    util::g_benchstats.block_read_disk_us.store(0);
    util::g_benchstats.block_decompress_us.store(0);
    util::g_benchstats.block_decompress_jobs.store(0);
    util::g_benchstats.block_prefetch_hit.store(0);
    util::g_benchstats.block_prefetch_wait_us.store(0);
    util::g_benchstats.coin_prefetch_prevouts.store(0);
    util::g_benchstats.coin_prefetch_misses.store(0);
    util::g_benchstats.coin_prefetch_lmdb_us.store(0);
    util::g_benchstats.coin_prefetch_decode_us.store(0);
    util::g_benchstats.coin_prefetch_warm_us.store(0);
    util::g_benchstats.blocks_connected.store(0);
}

void MineManyPrevoutBlock(TestChain100Setup& setup, const int num_txs)
{
    const int tip_height{WITH_LOCK(Assert(setup.m_node.chainman)->GetMutex(),
                                    return Assert(setup.m_node.chainman)->ActiveChain().Height())};
    const CScript script_pub_key{CScript() << ToByteVector(setup.coinbaseKey.GetPubKey()) << OP_CHECKSIG};
    std::vector<CMutableTransaction> txns;
    txns.reserve(static_cast<size_t>(num_txs));
    int added{0};
    for (size_t idx = 0; idx < setup.m_coinbase_txns.size() && added < num_txs; ++idx) {
        const int coin_height{static_cast<int>(idx) + 1};
        if (coin_height + COINBASE_MATURITY > tip_height) continue;
        const auto& coinbase{setup.m_coinbase_txns[idx]};
        const auto [mtx, _]{setup.CreateValidTransaction({coinbase},
                                                         {COutPoint{coinbase->GetHash(), 0}},
                                                         coin_height,
                                                         {setup.coinbaseKey},
                                                         {{50 * COIN, CScript() << OP_TRUE}},
                                                         std::nullopt,
                                                         std::nullopt)};
        txns.push_back(mtx);
        ++added;
    }
    BOOST_REQUIRE_EQUAL(added, num_txs);
    setup.CreateAndProcessBlock(txns, script_pub_key);
}

void MineLargeCompressedBlock(TestChain100Setup& setup)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.resize(1);
    tx.vout[0].nValue = 50 * COIN;
    std::vector<uint8_t> payload(512 * 1024);
    for (size_t off = 0; off < payload.size(); off += 32) {
        const size_t chunk{std::min<size_t>(32, payload.size() - off)};
        GetRandBytes({payload.data() + off, chunk});
    }
    tx.vout[0].scriptPubKey = CScript() << OP_RETURN << payload;
    const CScript script_pub_key{CScript() << ToByteVector(setup.coinbaseKey.GetPubKey()) << OP_CHECKSIG};
    setup.CreateAndProcessBlock({tx}, script_pub_key);
}

std::vector<uint256> ReplaySegment(TestChain100Setup& setup,
                                   int start_height,
                                   int end_height,
                                   int decompress_workers,
                                   int prefetch_workers)
{
    auto& chainman{*setup.m_node.chainman};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    auto& blockman{chainman.m_blockman};

    static_cast<TestChainstateManager&>(chainman).ResetIbd();
    if (decompress_workers > 0 || prefetch_workers > 0) {
        blockman.m_importing.store(true, std::memory_order_relaxed);
    } else {
        blockman.m_importing.store(false, std::memory_order_relaxed);
    }
    chainman.UpdateIBDStatus();

    blockman.ConfigureDecompressPool(decompress_workers);
    if (decompress_workers == 0) {
        blockman.SetIbdParallelReadsAllowed(blockman.IbdParallelReadsAllowed());
    }
    chainstate.CoinsDB().SetCoinPrefetchWorkers(prefetch_workers);

    BlockValidationState state;
    std::vector<uint256> tips;
    tips.reserve(static_cast<size_t>(end_height - start_height));
    LOCK2(chainman.GetMutex(), ::cs_main);
    while (chainstate.m_chain.Height() < end_height) {
        const int height_before{chainstate.m_chain.Height()};
        BOOST_REQUIRE(chainstate.ActivateBestChain(state, nullptr));
        BOOST_REQUIRE_GT(chainstate.m_chain.Height(), height_before);
        tips.push_back(chainstate.CoinsTip().GetBestBlock());
    }
    return tips;
}

void DisconnectToHeight(TestChain100Setup& setup, int height)
{
    auto& chainman{*setup.m_node.chainman};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BlockValidationState state;
    LOCK2(chainman.GetMutex(), ::cs_main);
    while (chainstate.m_chain.Height() > height) {
        BOOST_REQUIRE(chainstate.DisconnectTip(state, nullptr));
    }
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(validation_segment_equivalence_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(warmcache_prefetch_matches_serial_fetch)
{
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true}, CoinsViewOptions{}};
    CCoinsViewCache seed{&db};
    const COutPoint outpoint{AddTestCoin(rng, seed)};
    seed.SetBestBlock(uint256::ONE);
    BOOST_REQUIRE(seed.Flush());

    std::vector<COutPoint> prevouts{outpoint};
    std::vector<PrefetchedCoin> prefetched;
    BOOST_REQUIRE(ParallelPrefetchCoins(db, prevouts, prefetched, 4));
    BOOST_REQUIRE_EQUAL(prefetched.size(), 1);

    CCoinsViewCacheProbe serial{&db};
    CCoinsViewCacheProbe parallel{&db};
    BOOST_REQUIRE(serial.GetCoin(outpoint).has_value());
    BOOST_CHECK(!serial.EntryIsDirty(outpoint));
    BOOST_CHECK(!serial.EntryIsFresh(outpoint));

    parallel.WarmCache(std::move(prefetched));
    const auto parallel_coin{parallel.GetCoin(outpoint)};
    BOOST_REQUIRE(parallel_coin.has_value());
    BOOST_CHECK(parallel_coin->out == serial.GetCoin(outpoint)->out);
    BOOST_CHECK(!parallel.EntryIsDirty(outpoint));
    BOOST_CHECK(!parallel.EntryIsFresh(outpoint));
}

BOOST_AUTO_TEST_CASE(warmcache_skips_spent_and_existing_entries)
{
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true}, CoinsViewOptions{}};
    CCoinsViewCache seed{&db};
    const COutPoint live{AddTestCoin(rng, seed)};
    const COutPoint cached{AddTestCoin(rng, seed)};
    const Coin live_coin{seed.GetCoin(live).value()};
    const Coin cached_coin{seed.GetCoin(cached).value()};
    seed.SetBestBlock(uint256::ONE);
    BOOST_REQUIRE(seed.Flush());

    CCoinsViewCache view{&db};
    view.WarmCache(std::vector<PrefetchedCoin>{{cached, cached_coin}});
    BOOST_REQUIRE(view.HaveCoinInCache(cached));

    Coin spent_coin{live_coin};
    spent_coin.Clear();
    view.WarmCache(std::vector<PrefetchedCoin>{{live, spent_coin}});
    BOOST_CHECK(!view.HaveCoinInCache(live));
}

BOOST_AUTO_TEST_CASE(warmcache_bulk_64_prevouts)
{
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true}, CoinsViewOptions{}};
    std::vector<COutPoint> prevouts;
    prevouts.reserve(64);
    for (int i = 0; i < 64; ++i) {
        CCoinsViewCache seed{&db};
        prevouts.push_back(AddTestCoin(rng, seed));
        seed.SetBestBlock(uint256::ONE);
        BOOST_REQUIRE(seed.Flush());
    }

    std::vector<PrefetchedCoin> prefetched;
    BOOST_REQUIRE(ParallelPrefetchCoins(db, prevouts, prefetched, 4));
    BOOST_REQUIRE_EQUAL(prefetched.size(), 64);

    CCoinsViewCache view{&db};
    view.WarmCache(std::move(prefetched));
    for (const COutPoint& outpoint : prevouts) {
        BOOST_REQUIRE(view.HaveCoinInCache(outpoint));
        BOOST_CHECK(view.GetCoin(outpoint).has_value());
    }
}

BOOST_AUTO_TEST_CASE(segment_replay_serial_vs_parallel)
{
    mineBlocks(100);
    MineLargeCompressedBlock(*this);
    MineManyPrevoutBlock(*this, 70);

    const int end_height{WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return Assert(m_node.chainman)->ActiveChain().Height())};
    const int start_height{end_height - 12};

    util::g_benchstats_enabled.store(true);
    ZeroBenchStats();

    DisconnectToHeight(*this, start_height);
    const auto serial_tips{ReplaySegment(*this, start_height, end_height, /*decompress_workers=*/0, /*prefetch_workers=*/0)};
    const uint64_t serial_decompress_jobs{util::g_benchstats.block_decompress_jobs.load()};
    const uint64_t serial_coin_prefetch{util::g_benchstats.coin_prefetch_prevouts.load()};

    ZeroBenchStats();
    DisconnectToHeight(*this, start_height);
    const auto parallel_tips{ReplaySegment(*this, start_height, end_height, /*decompress_workers=*/4, /*prefetch_workers=*/4)};
    const uint64_t parallel_decompress_jobs{util::g_benchstats.block_decompress_jobs.load()};
    const uint64_t parallel_coin_prefetch{util::g_benchstats.coin_prefetch_prevouts.load()};

    BOOST_REQUIRE_EQUAL(serial_tips.size(), parallel_tips.size());
    for (size_t i = 0; i < serial_tips.size(); ++i) {
        BOOST_CHECK_EQUAL(serial_tips[i], parallel_tips[i]);
    }
    BOOST_CHECK_EQUAL(serial_tips.back(), parallel_tips.back());
    BOOST_CHECK(m_node.chainman->m_blockman.IbdParallelReadsAllowed());
    if (parallel_decompress_jobs > 0) {
        BOOST_CHECK_GT(parallel_decompress_jobs, serial_decompress_jobs);
    }
    BOOST_CHECK_GT(parallel_coin_prefetch, serial_coin_prefetch);

    m_node.chainman->m_blockman.m_importing.store(false, std::memory_order_relaxed);
    m_node.chainman->UpdateIBDStatus();
}

BOOST_AUTO_TEST_CASE(connect_block_many_prevouts_matches_serial_with_prefetch)
{
    static_cast<TestChainstateManager&>(*m_node.chainman).ResetIbd();
    m_node.chainman->m_blockman.m_importing.store(true, std::memory_order_relaxed);
    m_node.chainman->UpdateIBDStatus();

    mineBlocks(100);
    MineManyPrevoutBlock(*this, 70);

    const int end_height{WITH_LOCK(Assert(m_node.chainman)->GetMutex(),
                                    return Assert(m_node.chainman)->ActiveChain().Height())};
    const uint256 expected_tip{WITH_LOCK(Assert(m_node.chainman)->GetMutex(),
                                          return Assert(m_node.chainman)->ActiveChainstate().CoinsTip().GetBestBlock())};

    DisconnectToHeight(*this, end_height - 1);

    uint256 serial_tip;
    {
        auto& chainman{*m_node.chainman};
        chainman.ActiveChainstate().CoinsDB().SetCoinPrefetchWorkers(0);
        BlockValidationState state;
        LOCK2(chainman.GetMutex(), ::cs_main);
        BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state, nullptr));
        serial_tip = chainman.ActiveChainstate().CoinsTip().GetBestBlock();
        BOOST_REQUIRE(chainman.ActiveChainstate().DisconnectTip(state, nullptr));
    }

    uint256 parallel_tip;
    {
        auto& chainman{*m_node.chainman};
        chainman.ActiveChainstate().CoinsDB().SetCoinPrefetchWorkers(16);
        BlockValidationState state;
        LOCK2(chainman.GetMutex(), ::cs_main);
        BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state, nullptr));
        parallel_tip = chainman.ActiveChainstate().CoinsTip().GetBestBlock();
    }

    BOOST_CHECK_EQUAL(serial_tip, expected_tip);
    BOOST_CHECK_EQUAL(parallel_tip, expected_tip);
    BOOST_CHECK_EQUAL(serial_tip, parallel_tip);

    m_node.chainman->m_blockman.m_importing.store(false, std::memory_order_relaxed);
    m_node.chainman->UpdateIBDStatus();
}

BOOST_AUTO_TEST_SUITE_END()
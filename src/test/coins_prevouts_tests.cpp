// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <dbwrapper.h>
#include <txdb.h>
#include <primitives/block.h>
#include <script/script.h>

#include <boost/test/unit_test.hpp>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>

BOOST_FIXTURE_TEST_SUITE(coins_prevouts_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(collect_block_prevouts_dedup_skip_cached_skip_coinbase)
{
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true}, CoinsViewOptions{}};
    CCoinsViewCache cache{&db};

    const COutPoint cached{AddTestCoin(rng, cache)};
    const COutPoint uncached_parent{AddTestCoin(rng, cache)};
    cache.SetBestBlock(uint256::ONE);
    BOOST_REQUIRE(cache.Flush());

    CCoinsViewCache view{&db};
    view.WarmCache(std::vector<PrefetchedCoin>{{cached, cache.GetCoin(cached).value()}});

    CMutableTransaction funded;
    funded.vin.emplace_back(CTxIn{cached});
    funded.vin.emplace_back(CTxIn{uncached_parent});
    funded.vin.emplace_back(CTxIn{uncached_parent});
    funded.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(CMutableTransaction{}));
    block.vtx.push_back(MakeTransactionRef(funded));

    const std::vector<COutPoint> prevouts{CollectBlockPrevouts(block, view)};
    BOOST_REQUIRE_EQUAL(prevouts.size(), 1);
    BOOST_CHECK(prevouts[0] == uncached_parent);
    BOOST_CHECK(!view.HaveCoinInCache(uncached_parent));
}

BOOST_AUTO_TEST_SUITE_END()
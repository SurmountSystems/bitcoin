// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>
#include <txdb.h>

#include <boost/test/unit_test.hpp>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>

BOOST_FIXTURE_TEST_SUITE(txdb_prefetch_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(is_lmdb_readers_full_error_matches_handle_lmdb_format)
{
    const dbwrapper_error readers_full{"Fatal LMDB error in read transaction begin: Environment maxreaders limit reached (-30790)"};
    BOOST_CHECK(IsLMDBReadersFullError(readers_full));
    const dbwrapper_error other{"Fatal LMDB error in read: some other failure (-30798)"};
    BOOST_CHECK(!IsLMDBReadersFullError(other));
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_falls_back_on_readers_full)
{
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true, .max_readers = 1}, CoinsViewOptions{}};

    std::vector<COutPoint> prevouts;
    prevouts.reserve(64);
    for (int i = 0; i < 64; ++i) {
        CCoinsViewCache seed{&db};
        AddTestCoin(rng, seed);
        seed.SetBestBlock(uint256::ONE);
        BOOST_REQUIRE(seed.Flush());
        prevouts.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), static_cast<uint32_t>(i)});
    }

    BOOST_REQUIRE_EQUAL(db.GetMaxReaders(), 1u);

    std::vector<PrefetchedCoin> out;
    const int workers{static_cast<int>(db.GetMaxReaders()) * 16};
    const bool ok{ParallelPrefetchCoins(db, prevouts, out, workers)};
    BOOST_REQUIRE(!ok);
    BOOST_CHECK(out.empty());
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_boundary_cases)
{
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true}, CoinsViewOptions{}};

    std::vector<COutPoint> empty;
    std::vector<PrefetchedCoin> out;
    BOOST_CHECK(!ParallelPrefetchCoins(db, empty, out, 4));

    CCoinsViewCache lone_cache{&db};
    const COutPoint lone{AddTestCoin(rng, lone_cache)};
    lone_cache.SetBestBlock(uint256::ONE);
    BOOST_REQUIRE(lone_cache.Flush());
    BOOST_CHECK(!ParallelPrefetchCoins(db, std::vector<COutPoint>{lone}, out, 1));

    std::vector<COutPoint> mixed;
    mixed.reserve(4);
    for (int i = 0; i < 2; ++i) {
        CCoinsViewCache row{&db};
        mixed.push_back(AddTestCoin(rng, row));
        row.SetBestBlock(uint256::ONE);
        BOOST_REQUIRE(row.Flush());
    }
    mixed.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 99});
    mixed.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 100});

    out.clear();
    BOOST_REQUIRE(ParallelPrefetchCoins(db, mixed, out, 2));
    BOOST_CHECK_EQUAL(out.size(), 2);
}

BOOST_AUTO_TEST_CASE(get_max_readers_reflects_db_params)
{
    CCoinsViewDB small{DBParams{.memory_only = true, .max_readers = 3}, CoinsViewOptions{}};
    BOOST_CHECK_EQUAL(small.GetMaxReaders(), 3u);
}

BOOST_AUTO_TEST_SUITE_END()
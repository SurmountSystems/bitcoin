// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <dbwrapper.h>
#include <txdb.h>

#include <boost/test/unit_test.hpp>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/benchstats.h>

#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(txdb_prefetch_tests, BasicTestingSetup)

namespace {

bool CoinsEqual(const Coin& a, const Coin& b)
{
    return a.nHeight == b.nHeight && a.fCoinBase == b.fCoinBase && a.out == b.out;
}

std::vector<COutPoint> SeedPrevouts(FastRandomContext& rng, CCoinsViewDB& db, const size_t count)
{
    std::vector<COutPoint> prevouts;
    prevouts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        CCoinsViewCache seed{&db};
        prevouts.push_back(AddTestCoin(rng, seed));
        seed.SetBestBlock(uint256::ONE);
        BOOST_REQUIRE(seed.Flush());
    }
    return prevouts;
}

//! Hold every LMDB reader slot open so parallel prefetch workers hit MDB_READERS_FULL.
//! Deterministic alternative to timing-dependent worker-gate synchronization (Workstream A
//! reader budgeting may allow prefetch to succeed when slots are not actually exhausted).
class ReaderSlotHog
{
    std::vector<std::thread> m_threads;
    std::atomic<int> m_acquired{0};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_release{false};
    const int m_count;

public:
    ReaderSlotHog(CCoinsViewDB& db, const COutPoint& probe, const int count) : m_count{count}
    {
        BOOST_REQUIRE(count > 0);
        m_threads.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            m_threads.emplace_back([&db, probe, this]() {
                // Do not use BOOST_* macros from worker threads (TSan: boost::unit_test_log is not thread-safe).
                if (!db.GetCoin(probe)) {
                    m_failed.store(true, std::memory_order_release);
                    return;
                }
                m_acquired.fetch_add(1, std::memory_order_release);
                while (!m_release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                db.ReleaseThreadLocalReadTxn();
            });
        }
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{30}};
        while (m_acquired.load(std::memory_order_acquire) < m_count) {
            BOOST_REQUIRE_MESSAGE(
                !m_failed.load(std::memory_order_acquire),
                "ReaderSlotHog thread failed to acquire LMDB read txn for probe coin");
            BOOST_REQUIRE_MESSAGE(
                std::chrono::steady_clock::now() < deadline,
                "ReaderSlotHog timed out waiting for " << m_count << " reader slots "
                << "(acquired=" << m_acquired.load() << "); LMDB reader hog thread may have failed");
            std::this_thread::yield();
        }
    }

    ~ReaderSlotHog()
    {
        m_release.store(true, std::memory_order_release);
        for (std::thread& thread : m_threads) {
            thread.join();
        }
    }
};

//! Exhaust the environment reader table, then run parallel prefetch (expects fallback).
bool ParallelPrefetchCoinsWithReaderContention(CCoinsViewDB& db,
                                               const std::vector<COutPoint>& prevouts,
                                               std::vector<PrefetchedCoin>& out,
                                               const int num_workers)
{
    BOOST_REQUIRE(!prevouts.empty());
    // Caller may hold a thread-local read txn (e.g. baseline GetCoin); release it so hog
    // threads can occupy the full maxreaders table without throwing MDB_READERS_FULL.
    db.ReleaseThreadLocalReadTxn();
    const int hog_count{static_cast<int>(db.GetMaxReaders())};
    ReaderSlotHog hog{db, prevouts.front(), hog_count};
    return ParallelPrefetchCoins(db, prevouts, out, num_workers);
}

} // namespace

BOOST_AUTO_TEST_CASE(is_lmdb_readers_full_error_matches_handle_lmdb_format)
{
    const dbwrapper_error readers_full{"Fatal LMDB error in read transaction begin: Environment maxreaders limit reached (-30790)"};
    BOOST_CHECK(IsLMDBReadersFullError(readers_full));
    const dbwrapper_error other{"Fatal LMDB error in read: some other failure (-30798)"};
    BOOST_CHECK(!IsLMDBReadersFullError(other));
}

BOOST_AUTO_TEST_CASE(compute_coin_prefetch_workers_boundary_cases)
{
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(8, 512), 8);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(16, 512), 8);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(2, 512), 2);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(1, 512), 0);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(8, 64), 0);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(8, COIN_PREFETCH_READER_RESERVE), 0);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(8, COIN_PREFETCH_READER_RESERVE + 1), 0);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(8, COIN_PREFETCH_READER_RESERVE + 3), 1);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(8, COIN_PREFETCH_READER_RESERVE + 2 * COIN_PREFETCH_SLOTS_PER_WORKER), 2);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(8, COIN_PREFETCH_READER_RESERVE + 4 * COIN_PREFETCH_SLOTS_PER_WORKER), 4);
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(8, COIN_PREFETCH_MIN_MAXREADERS), 2);
}

BOOST_AUTO_TEST_CASE(reclaim_stale_readers_fresh_env)
{
    CCoinsViewDB db{DBParams{.memory_only = true}, CoinsViewOptions{}};
    BOOST_CHECK_EQUAL(db.ReclaimStaleReaders(), 0);
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_cooldown_blocks_entry)
{
    ResetCoinPrefetchCooldownForTest();
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true}, CoinsViewOptions{}};
    const std::vector<COutPoint> prevouts{SeedPrevouts(rng, db, 8)};

    SetCoinPrefetchCooldownForTest();
    BOOST_CHECK(CoinPrefetchInCooldownForTest());

    std::vector<PrefetchedCoin> out;
    BOOST_CHECK(!ParallelPrefetchCoins(db, prevouts, out, 4));
    BOOST_CHECK(out.empty());

    ResetCoinPrefetchCooldownForTest();
    BOOST_CHECK(!CoinPrefetchInCooldownForTest());
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_falls_back_on_readers_full)
{
    ResetCoinPrefetchCooldownForTest();
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true, .max_readers = 2}, CoinsViewOptions{}};
    const std::vector<COutPoint> prevouts{SeedPrevouts(rng, db, 64)};

    BOOST_REQUIRE_EQUAL(db.GetMaxReaders(), 2u);
    // Production caps workers via ComputeCoinPrefetchWorkers; this exercises the
    // MDB_READERS_FULL fallback when every reader slot is already held.
    BOOST_CHECK_EQUAL(ComputeCoinPrefetchWorkers(8, db.GetMaxReaders()), 0);

    std::vector<PrefetchedCoin> out;
    const bool ok{ParallelPrefetchCoinsWithReaderContention(db, prevouts, out, 4)};
    BOOST_REQUIRE(!ok);
    BOOST_CHECK(out.empty());
    BOOST_CHECK(CoinPrefetchInCooldownForTest());

    ResetCoinPrefetchCooldownForTest();
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_serial_fallback_returns_correct_coins)
{
    ResetCoinPrefetchCooldownForTest();
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true, .max_readers = 2}, CoinsViewOptions{}};
    const std::vector<COutPoint> prevouts{SeedPrevouts(rng, db, 32)};

    std::map<COutPoint, Coin> baseline;
    for (const COutPoint& prevout : prevouts) {
        const std::optional<Coin> coin{db.GetCoin(prevout)};
        BOOST_REQUIRE(coin);
        baseline.emplace(prevout, *coin);
    }

    std::vector<PrefetchedCoin> out;
    BOOST_REQUIRE(!ParallelPrefetchCoinsWithReaderContention(db, prevouts, out, 8));
    BOOST_CHECK(out.empty());

    for (const auto& [prevout, expected] : baseline) {
        const std::optional<Coin> coin{db.GetCoin(prevout)};
        BOOST_REQUIRE(coin);
        BOOST_CHECK(CoinsEqual(*coin, expected));
    }

    ResetCoinPrefetchCooldownForTest();
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_readers_full_increments_benchstats)
{
    ResetCoinPrefetchCooldownForTest();
    util::g_benchstats_enabled.store(true);
    util::g_benchstats.coin_prefetch_readers_full.store(0);

    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true, .max_readers = 2}, CoinsViewOptions{}};
    const std::vector<COutPoint> prevouts{SeedPrevouts(rng, db, 16)};

    std::vector<PrefetchedCoin> out;
    BOOST_REQUIRE(!ParallelPrefetchCoinsWithReaderContention(db, prevouts, out, 4));
    BOOST_CHECK_EQUAL(util::g_benchstats.coin_prefetch_readers_full.load(), 1u);
    BOOST_CHECK(CoinPrefetchInCooldownForTest());

    util::g_benchstats_enabled.store(false);
    util::g_benchstats.coin_prefetch_readers_full.store(0);
    ResetCoinPrefetchCooldownForTest();
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_recovers_after_cooldown_reset)
{
    ResetCoinPrefetchCooldownForTest();
    FastRandomContext rng;
    CCoinsViewDB tight{DBParams{.memory_only = true, .max_readers = 2}, CoinsViewOptions{}};
    const std::vector<COutPoint> prevouts{SeedPrevouts(rng, tight, 32)};

    std::vector<PrefetchedCoin> out;
    BOOST_REQUIRE(!ParallelPrefetchCoinsWithReaderContention(tight, prevouts, out, 4));
    BOOST_CHECK(CoinPrefetchInCooldownForTest());

    ResetCoinPrefetchCooldownForTest();
    CCoinsViewDB roomy{DBParams{.memory_only = true, .max_readers = 32}, CoinsViewOptions{}};
    const std::vector<COutPoint> roomy_prevouts{SeedPrevouts(rng, roomy, 32)};
    out.clear();
    BOOST_REQUIRE(ParallelPrefetchCoins(roomy, roomy_prevouts, out, 2));
    BOOST_CHECK_EQUAL(out.size(), roomy_prevouts.size());
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_partial_budget_contention)
{
    ResetCoinPrefetchCooldownForTest();
    FastRandomContext rng;
    // Exhaust both reader slots, then overlap two prefetch workers.
    CCoinsViewDB db{DBParams{.memory_only = true, .max_readers = 2}, CoinsViewOptions{}};
    const std::vector<COutPoint> prevouts{SeedPrevouts(rng, db, 16)};

    std::vector<PrefetchedCoin> out;
    BOOST_REQUIRE(!ParallelPrefetchCoinsWithReaderContention(db, prevouts, out, 2));
    BOOST_CHECK(CoinPrefetchInCooldownForTest());

    ResetCoinPrefetchCooldownForTest();
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_reserve_disables_workers_at_max_readers_64)
{
    ResetCoinPrefetchCooldownForTest();
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true, .max_readers = 64}, CoinsViewOptions{}};
    const std::vector<COutPoint> prevouts{SeedPrevouts(rng, db, 256)};

    const int workers{ComputeCoinPrefetchWorkers(8, db.GetMaxReaders())};
    BOOST_CHECK_EQUAL(workers, 0);
    BOOST_CHECK_EQUAL(db.GetMaxReaders(), 64u);

    std::vector<PrefetchedCoin> out;
    BOOST_CHECK(!ParallelPrefetchCoins(db, prevouts, out, workers));

    for (const COutPoint& prevout : prevouts) {
        const std::optional<Coin> coin{db.GetCoin(prevout)};
        BOOST_REQUIRE(coin);
        BOOST_CHECK(!coin->IsSpent());
    }
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_repeated_success_no_reader_leak)
{
    ResetCoinPrefetchCooldownForTest();
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.memory_only = true, .max_readers = 32}, CoinsViewOptions{}};
    const std::vector<COutPoint> prevouts{SeedPrevouts(rng, db, 16)};

    for (int round = 0; round < 20; ++round) {
        std::vector<PrefetchedCoin> out;
        BOOST_REQUIRE_MESSAGE(ParallelPrefetchCoins(db, prevouts, out, 2),
                              "prefetch failed on round " << round);
        BOOST_CHECK_EQUAL(out.size(), prevouts.size());
    }
}

BOOST_AUTO_TEST_CASE(parallel_prefetch_boundary_cases)
{
    ResetCoinPrefetchCooldownForTest();
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
    std::map<COutPoint, Coin> baseline;
    mixed.reserve(4);
    for (int i = 0; i < 2; ++i) {
        CCoinsViewCache row{&db};
        const COutPoint outpoint{AddTestCoin(rng, row)};
        row.SetBestBlock(uint256::ONE);
        BOOST_REQUIRE(row.Flush());
        mixed.push_back(outpoint);
        const std::optional<Coin> coin{db.GetCoin(outpoint)};
        BOOST_REQUIRE(coin);
        baseline.emplace(outpoint, *coin);
    }
    mixed.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 99});
    mixed.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 100});

    out.clear();
    BOOST_REQUIRE(ParallelPrefetchCoins(db, mixed, out, 2));
    BOOST_CHECK_EQUAL(out.size(), 2);
    for (const PrefetchedCoin& entry : out) {
        const auto it{baseline.find(entry.outpoint)};
        BOOST_REQUIRE(it != baseline.end());
        BOOST_CHECK(CoinsEqual(entry.coin, it->second));
    }
}

BOOST_AUTO_TEST_CASE(get_max_readers_reflects_db_params)
{
    CCoinsViewDB small{DBParams{.memory_only = true, .max_readers = 3}, CoinsViewOptions{}};
    BOOST_CHECK_EQUAL(small.GetMaxReaders(), 3u);
}

BOOST_AUTO_TEST_SUITE_END()
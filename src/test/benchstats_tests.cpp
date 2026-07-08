// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/benchstats.h>

#include <logging.h>
#include <test/util/setup_common.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <optional>
#include <string>
#include <thread>

BOOST_FIXTURE_TEST_SUITE(benchstats_tests, BasicTestingSetup)

struct BenchStatsLogSetup : BasicTestingSetup {
    fs::path prev_log_path;
    fs::path tmp_log_path;
    bool prev_reopen_file;
    bool prev_print_to_file;
    BCLog::Level prev_log_level;
    BCLog::CategoryMask prev_category_mask;

    BenchStatsLogSetup()
        : prev_log_path{LogInstance().m_file_path},
          tmp_log_path{m_args.GetDataDirBase() / "benchstats_debug.log"},
          prev_reopen_file{LogInstance().m_reopen_file},
          prev_print_to_file{LogInstance().m_print_to_file},
          prev_log_level{LogInstance().LogLevel()},
          prev_category_mask{LogInstance().GetCategoryMask()}
    {
        LogInstance().m_file_path = tmp_log_path;
        LogInstance().m_reopen_file = true;
        LogInstance().m_print_to_file = true;
        LogInstance().m_log_timestamps = false;
        LogInstance().SetLogLevel(BCLog::Level::Debug);
        LogInstance().DisableCategory(BCLog::LogFlags::ALL);
        LogInstance().EnableCategory(BCLog::BENCH);
        LogPrintf("benchstats test log reopen\n");
    }

    ~BenchStatsLogSetup()
    {
        util::g_benchstats_enabled.store(false);
        LogInstance().m_file_path = prev_log_path;
        LogInstance().m_print_to_file = prev_print_to_file;
        LogInstance().m_reopen_file = prev_reopen_file;
        LogInstance().SetLogLevel(prev_log_level);
        LogInstance().DisableCategory(BCLog::LogFlags::ALL);
        LogInstance().EnableCategory(BCLog::LogFlags{prev_category_mask});
    }

    std::string ReadLog() const
    {
        std::ifstream in{tmp_log_path};
        return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
};

BOOST_FIXTURE_TEST_CASE(maybe_log_benchstats_every_1000_blocks, BenchStatsLogSetup)
{
    util::g_benchstats_enabled.store(true);
    util::g_benchstats.blocks_connected.store(0);
    util::g_benchstats.block_read_disk_us.store(0);

    for (uint64_t i = 0; i < 999; ++i) {
        util::g_benchstats.blocks_connected.fetch_add(1, std::memory_order_relaxed);
        util::MaybeLogBenchStats();
    }
    BOOST_CHECK(ReadLog().find("benchstats:") == std::string::npos);

    util::g_benchstats.blocks_connected.fetch_add(1, std::memory_order_relaxed);
    util::g_benchstats.block_read_disk_us.store(5000, std::memory_order_relaxed);
    util::g_benchstats.cs_main_hold_us.store(120500, std::memory_order_relaxed);
    util::g_benchstats.flush_mutex_wait_us.store(3200, std::memory_order_relaxed);
    util::g_benchstats.blkidx_mutex_wait_us.store(1100, std::memory_order_relaxed);
    util::g_benchstats.abc_idle_us.store(50000, std::memory_order_relaxed);
    util::g_benchstats.script_jobs.store(8000, std::memory_order_relaxed);
    util::g_benchstats.script_done.store(8000, std::memory_order_relaxed);
    util::MaybeLogBenchStats();

    const std::string log{ReadLog()};
    BOOST_CHECK(log.find("benchstats:") != std::string::npos);
    BOOST_CHECK(log.find("blocks=1000") != std::string::npos);
    BOOST_CHECK(log.find("wait cs_main_hold=120.5ms") != std::string::npos);
    BOOST_CHECK(log.find("flush_mutex_wait=3.2ms") != std::string::npos);
    BOOST_CHECK(log.find("blkidx_mutex_wait=1.1ms") != std::string::npos);
    BOOST_CHECK(log.find("abc_idle=50.0ms") != std::string::npos);
    BOOST_CHECK(log.find("script_jobs=8000") != std::string::npos);
    BOOST_CHECK(log.find("script_done=8000") != std::string::npos);
    BOOST_CHECK_EQUAL(util::g_benchstats.blocks_connected.load(), 0u);
    BOOST_CHECK_EQUAL(util::g_benchstats.cs_main_hold_us.load(), 0u);
}

BOOST_FIXTURE_TEST_CASE(maybe_log_benchstats_without_debug_bench, BenchStatsLogSetup)
{
    LogInstance().DisableCategory(BCLog::BENCH);
    util::g_benchstats_enabled.store(true);
    util::g_benchstats.blocks_connected.store(0);

    for (uint64_t i = 0; i < 1000; ++i) {
        util::g_benchstats.blocks_connected.fetch_add(1, std::memory_order_relaxed);
        util::MaybeLogBenchStats();
    }

    const std::string log{ReadLog()};
    BOOST_CHECK(log.find("benchstats:") != std::string::npos);
    BOOST_CHECK(log.find("blocks=1000") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(benchstats_cs_main_hold_pause_resume, BasicTestingSetup)
{
    util::g_benchstats_enabled.store(true);
    util::g_benchstats.cs_main_hold_us.store(0, std::memory_order_relaxed);

    {
        util::BenchStatsCsMainHold hold;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        hold.Pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        hold.Resume();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    const uint64_t us{util::g_benchstats.cs_main_hold_us.load()};
    BOOST_CHECK(us >= 25'000);
    BOOST_CHECK(us < 80'000);
}

BOOST_FIXTURE_TEST_CASE(benchstats_cs_main_hold_outer_reuse_no_double_count, BasicTestingSetup)
{
    util::g_benchstats_enabled.store(true);
    util::g_benchstats.cs_main_hold_us.store(0, std::memory_order_relaxed);

    auto simulate_flush_cs_main_hold = [](util::BenchStatsCsMainHold* outer) {
        std::optional<util::BenchStatsCsMainHold> local;
        if (outer == nullptr) {
            local.emplace();
        }
        util::BenchStatsCsMainHold* const hold{outer != nullptr ? outer : &*local};
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        hold->Pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        hold->Resume();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    {
        util::BenchStatsCsMainHold outer;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        simulate_flush_cs_main_hold(&outer);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const uint64_t us{util::g_benchstats.cs_main_hold_us.load()};
    // ~30ms held (10+10 outer + 10 nested), not ~90ms if a stray local holder also accumulated I/O.
    BOOST_CHECK(us >= 25'000);
    BOOST_CHECK(us < 55'000);
}

BOOST_FIXTURE_TEST_CASE(benchstats_cs_main_hold_disabled_no_op, BasicTestingSetup)
{
    util::g_benchstats_enabled.store(false);
    util::g_benchstats.cs_main_hold_us.store(0, std::memory_order_relaxed);

    {
        util::BenchStatsCsMainHold hold;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        hold.Pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        hold.Resume();
    }

    BOOST_CHECK_EQUAL(util::g_benchstats.cs_main_hold_us.load(), 0u);
}

BOOST_FIXTURE_TEST_CASE(benchstats_abc_idle_advances_baseline, BasicTestingSetup)
{
    util::g_benchstats_enabled.store(true);
    util::g_benchstats.abc_idle_us.store(0, std::memory_order_relaxed);
    util::BenchStatsRecordConnectTipEnd();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    util::BenchStatsRecordAbcIdle();
    const uint64_t first{util::g_benchstats.abc_idle_us.load()};
    util::BenchStatsRecordAbcIdle();
    const uint64_t second{util::g_benchstats.abc_idle_us.load()};
    BOOST_CHECK(first >= 40'000);
    BOOST_CHECK(second - first < 5'000);
}

BOOST_FIXTURE_TEST_CASE(benchstats_add_gated_when_disabled, BenchStatsLogSetup)
{
    util::g_benchstats_enabled.store(false);
    util::g_benchstats.script_jobs.store(0, std::memory_order_relaxed);
    util::BenchStatsAdd(util::g_benchstats.script_jobs, 42);
    util::BenchStatsInc(util::g_benchstats.script_done);
    BOOST_CHECK_EQUAL(util::g_benchstats.script_jobs.load(), 0u);
    BOOST_CHECK_EQUAL(util::g_benchstats.script_done.load(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
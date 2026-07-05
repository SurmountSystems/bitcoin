// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <compress/zstd.h>
#include <dbwrapper.h>
#include <test/util/coins.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

namespace {
void PopulateDirtyCoins(FastRandomContext& rng, CCoinsViewCache& cache, const uint256& hash_block, int count)
{
    cache.SetBestBlock(hash_block);
    for (int i = 0; i < count; ++i) {
        AddTestCoin(rng, cache);
    }
}

void CheckPartialAndFinalSync(const std::vector<bool>& calls, bool expect_final_sync)
{
    BOOST_REQUIRE(!calls.empty());
    for (size_t i = 0; i + 1 < calls.size(); ++i) {
        BOOST_CHECK_MESSAGE(!calls[i], strprintf("partial batch %u should not sync", i));
    }
    BOOST_CHECK_EQUAL(calls.back(), expect_final_sync);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(txdb_sync_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(coinsviewdb_writebatch_sync_flags)
{
    auto& sync_log = dbwrapper_private::TestWriteBatchSyncLog();
    sync_log.active = true;

    const uint256 hash_block = m_rng.rand256();

    CoinsViewOptions options;
    options.batch_write_bytes = 1; // force partial batches
    options.utxo_zstd = false;

    CCoinsViewDB db{{.path = "sync-test", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
    CCoinsViewCache cache{&db};

    PopulateDirtyCoins(m_rng, cache, hash_block, 8);

    sync_log.Reset();
    BOOST_REQUIRE(cache.Flush());
    CheckPartialAndFinalSync(sync_log.calls, /*expect_final_sync=*/false);

    PopulateDirtyCoins(m_rng, cache, hash_block, 8);
    db.SetSyncFinalBatch(true);
    sync_log.Reset();
    BOOST_REQUIRE(cache.Flush());
    CheckPartialAndFinalSync(sync_log.calls, /*expect_final_sync=*/true);

    // sync_final_batch is cleared after BatchWrite even when set explicitly
    PopulateDirtyCoins(m_rng, cache, hash_block, 4);
    sync_log.Reset();
    BOOST_REQUIRE(cache.Flush());
    CheckPartialAndFinalSync(sync_log.calls, /*expect_final_sync=*/false);
}

BOOST_AUTO_TEST_CASE(coinsviewdb_lmdbsync_writes_all_batches)
{
    auto& sync_log = dbwrapper_private::TestWriteBatchSyncLog();
    sync_log.active = true;

    const uint256 hash_block = m_rng.rand256();

    CoinsViewOptions options;
    options.batch_write_bytes = 1;
    options.utxo_zstd = false;
    options.lmdbsync = true;

    CCoinsViewDB db{{.path = "lmdbsync-test", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
    CCoinsViewCache cache{&db};

    PopulateDirtyCoins(m_rng, cache, hash_block, 8);

    sync_log.Reset();
    BOOST_REQUIRE(cache.Flush());
    BOOST_REQUIRE_GE(sync_log.calls.size(), 2U);
    for (size_t i = 0; i < sync_log.calls.size(); ++i) {
        BOOST_CHECK_MESSAGE(sync_log.calls[i], strprintf("batch %u should sync with -lmdbsync", i));
    }

    sync_log.active = false;
}

BOOST_AUTO_TEST_CASE(coinsviewdb_parallel_partial_batch_sync_flags)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    auto& sync_log = dbwrapper_private::TestWriteBatchSyncLog();
    sync_log.active = true;

    const uint256 hash_block = m_rng.rand256();

    CoinsViewOptions options;
    options.batch_write_bytes = 1; // force partial batches
    options.utxo_zstd = true;
    options.utxo_zstd_level = DEFAULT_UTXO_ZSTD_LEVEL;
    options.utxo_zstd_dict_path = dict_path;
    options.utxo_encode_workers = 4;

    CCoinsViewDB db{{.path = "parallel-sync-test", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
    CCoinsViewCache cache{&db};

    PopulateDirtyCoins(m_rng, cache, hash_block, 300);

    sync_log.Reset();
    BOOST_REQUIRE(cache.Flush());
    BOOST_REQUIRE_GE(sync_log.calls.size(), 2U);
    CheckPartialAndFinalSync(sync_log.calls, /*expect_final_sync=*/false);

    PopulateDirtyCoins(m_rng, cache, hash_block, 300);
    db.SetSyncFinalBatch(true);
    sync_log.Reset();
    BOOST_REQUIRE(cache.Flush());
    CheckPartialAndFinalSync(sync_log.calls, /*expect_final_sync=*/true);

    sync_log.active = false;
}

//! Regression: ALWAYS flush re-asserts sync_final_batch before each snapshot write attempt.
BOOST_AUTO_TEST_CASE(flush_snapshot_sync_final_batch_per_retry)
{
    auto& sync_log = dbwrapper_private::TestWriteBatchSyncLog();
    sync_log.active = true;

    const uint256 hash_block = m_rng.rand256();

    CoinsViewOptions options;
    options.batch_write_bytes = 1;
    options.utxo_zstd = false;

    CCoinsViewDB db{{.path = "snapshot-sync-retry", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
    CCoinsViewCache cache{&db};

    PopulateDirtyCoins(m_rng, cache, hash_block, 8);
    CoinsFlushSnapshot snapshot{cache.CaptureFlushSnapshot(/*will_erase=*/false)};

    db.SetSyncFinalBatch(true);
    sync_log.Reset();
    BOOST_REQUIRE(db.BatchWriteFromSnapshot(snapshot));
    CheckPartialAndFinalSync(sync_log.calls, /*expect_final_sync=*/true);

    // Mimic validation.cpp stale-retry: re-assert before the next out-of-lock write.
    AddTestCoin(m_rng, cache);
    const CoinsFlushSnapshot snapshot2{cache.CaptureFlushSnapshot(/*will_erase=*/false)};
    db.SetSyncFinalBatch(true);
    sync_log.Reset();
    BOOST_REQUIRE(db.BatchWriteFromSnapshot(snapshot2));
    CheckPartialAndFinalSync(sync_log.calls, /*expect_final_sync=*/true);

    sync_log.active = false;
}

BOOST_AUTO_TEST_SUITE_END()
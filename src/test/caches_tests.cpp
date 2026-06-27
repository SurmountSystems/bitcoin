// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#include <common/args.h>
#include <node/caches.h>
#include <node/dbcache.h>
#include <util/byte_units.h>
#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

using namespace node;

namespace {
void CheckDbCacheWarnThreshold(uint64_t threshold, uint64_t total_ram, std::optional<uint64_t> reserved_mib = {})
{
    BOOST_CHECK(!ShouldWarnOversizedDbCache(threshold, total_ram, reserved_mib));
    BOOST_CHECK( ShouldWarnOversizedDbCache(threshold + 1, total_ram, reserved_mib));
}
} // namespace

BOOST_AUTO_TEST_SUITE(caches_tests)

BOOST_AUTO_TEST_CASE(default_dbcache_formula_by_total_ram)
{
    BOOST_CHECK(FALLBACK_RAM_BYTES >= 1_GiB);
    for (const auto& [total_ram, expected] : std::array<std::pair<uint64_t, uint64_t>, 4>{{
        {512_MiB, MIN_DEFAULT_DBCACHE},
        {1_GiB, MIN_DEFAULT_DBCACHE},
        {DEFAULT_RESERVED_RAM - 1, MIN_DEFAULT_DBCACHE},
        {DEFAULT_RESERVED_RAM, MIN_DEFAULT_DBCACHE}
    }}) {
        BOOST_CHECK_EQUAL(GetDefaultDbCacheIbd(total_ram), expected);
        BOOST_CHECK_EQUAL(GetDefaultDbCacheSynced(total_ram), expected);
    }

    BOOST_CHECK_EQUAL(GetDefaultDbCacheIbd(3_GiB), 640_MiB);
    BOOST_CHECK_EQUAL(GetDefaultDbCacheSynced(3_GiB), 256_MiB);

    if constexpr (SIZE_MAX > UINT32_MAX) {
        for (const auto& [total_ram_64, expected_ibd, expected_synced] : std::array<std::tuple<uint64_t, uint64_t, uint64_t>, 3>{{
            {8_GiB, 3840_MiB, 1536_MiB},
            {96_GiB, MAX_DEFAULT_DBCACHE, 24064_MiB},
            {128_GiB, MAX_DEFAULT_DBCACHE, 32256_MiB}
        }}) {
            BOOST_CHECK_EQUAL(GetDefaultDbCacheIbd(total_ram_64), expected_ibd);
            BOOST_CHECK_EQUAL(GetDefaultDbCacheSynced(total_ram_64), expected_synced);
        }
    }
}

BOOST_AUTO_TEST_CASE(default_dbcache_uses_current_total_ram)
{
    BOOST_CHECK_EQUAL(GetDefaultDBCache(), GetDefaultDBCache(GetTotalRam()));
}

BOOST_AUTO_TEST_CASE(reserved_ram_override)
{
    const uint64_t reserved_4g{4_GiB};
    const std::optional<uint64_t> reserved_mib{reserved_4g / 1_MiB};
    BOOST_CHECK_EQUAL(GetReservedRamBytes(reserved_mib), reserved_4g);
    BOOST_CHECK_EQUAL(GetDefaultDbCacheIbd(96_GiB, reserved_mib), MAX_DEFAULT_DBCACHE);
    BOOST_CHECK_EQUAL(GetDefaultDbCacheSynced(96_GiB, reserved_mib), 23552_MiB);
}

BOOST_AUTO_TEST_CASE(oversized_dbcache_warning)
{
    BOOST_CHECK(!ShouldWarnOversizedDbCache(MIN_DBCACHE_BYTES, 1_GiB));

    // Below reserved RAM the auto default dominates (headroom is zero).
    CheckDbCacheWarnThreshold(GetDefaultDBCache(1_GiB), 1_GiB);
    CheckDbCacheWarnThreshold(GetDefaultDBCache(DEFAULT_RESERVED_RAM), DEFAULT_RESERVED_RAM);

    // Above reserved RAM the warning fires at 75% of the headroom.
    CheckDbCacheWarnThreshold(((3_GiB - DEFAULT_RESERVED_RAM) / 4) * 3, 3_GiB);

    for (const auto total_ram : {8_GiB, 16_GiB, 32_GiB}) {
        CheckDbCacheWarnThreshold(((total_ram - DEFAULT_RESERVED_RAM) / 4) * 3, total_ram);
    }
}

BOOST_AUTO_TEST_CASE(default_dbcache_never_warns)
{
    for (const auto total_ram : {1_GiB, 2_GiB, 3_GiB}) {
        BOOST_CHECK(!ShouldWarnOversizedDbCache(GetDefaultDBCache(total_ram), total_ram));
    }

    for (const auto total_ram : {4_GiB, 8_GiB, 16_GiB, 32_GiB}) {
        BOOST_CHECK(!ShouldWarnOversizedDbCache(GetDefaultDBCache(total_ram), total_ram));
    }
}

BOOST_AUTO_TEST_CASE(should_shrink_cache_on_ibd_exit)
{
    ArgsManager args;
    BOOST_CHECK(ShouldShrinkCacheOnIbdExit(args));

    args.ForceSetArg("-dbcache", "1024");
    BOOST_CHECK(!ShouldShrinkCacheOnIbdExit(args));

    args.ClearArgs();
    args.ForceSetArg("-dbcache-ibd", "4096");
    args.ForceSetArg("-dbcache-synced", "4096");
    BOOST_CHECK(!ShouldShrinkCacheOnIbdExit(args));
}

BOOST_AUTO_TEST_CASE(cache_override_clamping)
{
    ArgsManager args;
    args.ForceSetArg("-dbcache", "1024");
    args.ForceSetArg("-coinscache", "1500");
    const auto sizes{CalculateCacheSizes(args).kernel};
    BOOST_CHECK_LE(sizes.block_tree_db + sizes.coins_db + sizes.coins, 1024_MiB);
    BOOST_CHECK_EQUAL(sizes.block_tree_db + sizes.coins_db + sizes.coins, 1024_MiB);

    args.ClearArgs();
    args.ForceSetArg("-dbcache", "1024");
    args.ForceSetArg("-blocktreecache", "900");
    args.ForceSetArg("-coinsdbcache", "900");
    args.ForceSetArg("-coinscache", "900");
    const auto all_overridden{CalculateCacheSizes(args).kernel};
    BOOST_CHECK_EQUAL(all_overridden.block_tree_db, 900_MiB);
    BOOST_CHECK_EQUAL(all_overridden.coins_db, 124_MiB);
    BOOST_CHECK_EQUAL(all_overridden.coins, 0);
}

BOOST_AUTO_TEST_SUITE_END()
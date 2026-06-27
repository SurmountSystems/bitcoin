// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/caches.h>

#include <common/args.h>
#include <common/system_ram.h>
#include <index/txindex.h>
#include <kernel/caches.h>
#include <logging.h>
#include <node/dbcache.h>
#include <node/interface_ui.h>
#include <tinyformat.h>
#include <util/byte_units.h>

#include <algorithm>
#include <string>

// Unlike for the UTXO database, for the txindex scenario the LMDB reader pool and
// OS page cache make a meaningful difference: https://github.com/bitcoin/bitcoin/pull/8273#issuecomment-229601991
//! Max memory allocated to tx index DB specific cache in bytes.
static constexpr size_t MAX_TX_INDEX_CACHE{1024_MiB};
//! Max memory allocated to all block filter index caches combined in bytes.
static constexpr size_t MAX_FILTER_INDEX_CACHE{1024_MiB};

namespace node {
namespace {
size_t MiBToBytes(int64_t mib)
{
    if (mib < 0) mib = 0;
    return SaturatingLeftShift<uint64_t>(mib, 20);
}

size_t ClampConfiguredDbCache(uint64_t db_cache_bytes) noexcept
{
    return std::max<size_t>(MIN_DBCACHE_BYTES, std::min(db_cache_bytes, MAX_DBCACHE_BYTES));
}

kernel::CacheSizeOverrides ReadCacheOverrides(const ArgsManager& args)
{
    kernel::CacheSizeOverrides overrides;
    if (auto value{args.GetIntArg("-blocktreecache")}) {
        if (*value > 0) overrides.block_tree = MiBToBytes(*value);
    }
    if (auto value{args.GetIntArg("-coinsdbcache")}) {
        if (*value > 0) overrides.coins_db = MiBToBytes(*value);
    }
    if (auto value{args.GetIntArg("-coinscache")}) {
        if (*value > 0) overrides.coins = MiBToBytes(*value);
    }
    return overrides;
}

size_t DefaultDbCacheForProfile(DbCacheProfile profile, uint64_t reserved_ram_bytes) noexcept
{
    const std::optional<uint64_t> reserved_mib{reserved_ram_bytes / 1_MiB};
    switch (profile) {
    case DbCacheProfile::SYNCED:
        return GetDefaultDbCacheSynced({}, reserved_mib);
    case DbCacheProfile::IBD:
        return GetDefaultDbCacheIbd({}, reserved_mib);
    } // no default
    return GetDefaultDbCacheIbd({}, reserved_mib);
}
} // namespace

size_t CalculateReservedRamBytes(const ArgsManager& args) noexcept
{
    if (auto reserved{args.GetIntArg("-reservedram")}) {
        if (*reserved > 0) return MiBToBytes(*reserved);
    }
    return DEFAULT_RESERVED_RAM;
}

size_t CalculateDbCacheBytes(const ArgsManager& args, DbCacheProfile profile) noexcept
{
    const size_t reserved_ram{CalculateReservedRamBytes(args)};
    const std::optional<uint64_t> reserved_mib{reserved_ram / 1_MiB};

    if (auto db_cache{args.GetIntArg("-dbcache")}) {
        return ClampConfiguredDbCache(MiBToBytes(*db_cache));
    }

    if (profile == DbCacheProfile::IBD) {
        if (auto ibd_cache{args.GetIntArg("-dbcache-ibd")}) {
            if (*ibd_cache > 0) return ClampConfiguredDbCache(MiBToBytes(*ibd_cache));
        }
        return DefaultDbCacheForProfile(DbCacheProfile::IBD, reserved_ram);
    }

    if (auto synced_cache{args.GetIntArg("-dbcache-synced")}) {
        if (*synced_cache > 0) return ClampConfiguredDbCache(MiBToBytes(*synced_cache));
    }
    return DefaultDbCacheForProfile(DbCacheProfile::SYNCED, reserved_ram);
}

bool ShouldShrinkCacheOnIbdExit(const ArgsManager& args) noexcept
{
    if (args.IsArgSet("-dbcache")) return false;
    return CalculateDbCacheBytes(args, DbCacheProfile::IBD) > CalculateDbCacheBytes(args, DbCacheProfile::SYNCED);
}

CacheSizes CalculateCacheSizes(const ArgsManager& args, size_t n_indexes, DbCacheProfile profile)
{
    size_t total_cache{CalculateDbCacheBytes(args, profile)};
    const auto overrides{ReadCacheOverrides(args)};

    IndexCacheSizes index_sizes;
    index_sizes.tx_index = std::min(total_cache / 8, args.GetBoolArg("-txindex", DEFAULT_TXINDEX) ? MAX_TX_INDEX_CACHE : 0);
    total_cache -= index_sizes.tx_index;
    if (n_indexes > 0) {
        size_t max_cache = std::min(total_cache / 8, MAX_FILTER_INDEX_CACHE);
        index_sizes.filter_index = max_cache / n_indexes;
        total_cache -= index_sizes.filter_index * n_indexes;
    }
    return {index_sizes, kernel::CacheSizes{total_cache, overrides}};
}

void LogOversizedDbCache(const ArgsManager& args) noexcept
{
    if (const auto total_ram{TryGetTotalRam()}) {
        const size_t db_cache{CalculateDbCacheBytes(args)};
        const std::optional<uint64_t> reserved_mib{CalculateReservedRamBytes(args) / 1_MiB};
        if (ShouldWarnOversizedDbCache(db_cache, *total_ram, reserved_mib)) {
            InitWarning(bilingual_str{tfm::format(_("A %s MiB dbcache may be too large for a system with only %s MiB of memory."),
                        db_cache / 1_MiB, *total_ram / 1_MiB)});
        }
    }
}

void LogAutoDbCacheSettings(const ArgsManager& args) noexcept
{
    const size_t reserved{CalculateReservedRamBytes(args)};
    const size_t usable{GetUsableRam(GetTotalRam(), reserved)};
    LogInfo("Automatically selected cache profiles based on %s system memory of %s MiB "
            "(usable %s MiB after %s MiB reserved): IBD=%s MiB (~62.5%%), synced=%s MiB (~25%%).",
            TryGetTotalRam() ? "detected" : "assumed",
            GetTotalRam() / 1_MiB,
            usable / 1_MiB,
            reserved / 1_MiB,
            GetDefaultDbCacheIbd(GetTotalRam(), reserved / 1_MiB) / 1_MiB,
            GetDefaultDbCacheSynced(GetTotalRam(), reserved / 1_MiB) / 1_MiB);
}

void LogSyncedCacheTarget(const ArgsManager& args) noexcept
{
    if (!ShouldShrinkCacheOnIbdExit(args)) return;
    const auto synced{CalculateCacheSizes(args, 0, DbCacheProfile::SYNCED).kernel};
    LogInfo("Will reduce kernel cache to synced profile after IBD: %.1f MiB total "
            "(coinstip %.1f MiB, coinsdb %.1f MiB, blocktree %.1f MiB).",
            (synced.coins + synced.coins_db + synced.block_tree_db) * (1.0 / 1024 / 1024),
            synced.coins * (1.0 / 1024 / 1024),
            synced.coins_db * (1.0 / 1024 / 1024),
            synced.block_tree_db * (1.0 / 1024 / 1024));
}
} // namespace node
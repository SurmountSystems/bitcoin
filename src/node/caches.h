// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CACHES_H
#define BITCOIN_NODE_CACHES_H

#include <kernel/caches.h>
#include <util/byte_units.h>

#include <cstddef>

class ArgsManager;

namespace node {
struct IndexCacheSizes {
    size_t tx_index{0};
    size_t filter_index{0};
};

struct CacheSizes {
    IndexCacheSizes index;
    kernel::CacheSizes kernel;
};

enum class DbCacheProfile {
    IBD,
    SYNCED,
};

size_t CalculateReservedRamBytes(const ArgsManager& args) noexcept;
size_t CalculateDbCacheBytes(const ArgsManager& args, DbCacheProfile profile = DbCacheProfile::IBD) noexcept;
bool ShouldShrinkCacheOnIbdExit(const ArgsManager& args) noexcept;
CacheSizes CalculateCacheSizes(const ArgsManager& args, size_t n_indexes = 0, DbCacheProfile profile = DbCacheProfile::IBD);

void LogOversizedDbCache(const ArgsManager& args) noexcept;
void LogAutoDbCacheSettings(const ArgsManager& args) noexcept;
void LogSyncedCacheTarget(const ArgsManager& args) noexcept;
} // namespace node

#endif // BITCOIN_NODE_CACHES_H
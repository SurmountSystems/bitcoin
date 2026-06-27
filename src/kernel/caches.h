// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_CACHES_H
#define BITCOIN_KERNEL_CACHES_H

#include <util/byte_units.h>

#include <algorithm>

//! Suggested default amount of cache reserved for the kernel (bytes)
static constexpr size_t DEFAULT_KERNEL_CACHE{450_MiB};

namespace kernel {
struct CacheSizeOverrides {
    //! Per-pool overrides in bytes; zero means use the computed default split.
    size_t block_tree{0};
    size_t coins_db{0};
    size_t coins{0};
};

struct CacheSizes {
    size_t block_tree_db{0};
    size_t coins_db{0};
    size_t coins{0};

    CacheSizes() = default;

    CacheSizes(size_t total_cache, const CacheSizeOverrides& overrides = {})
    {
        size_t remaining{total_cache};

        block_tree_db = overrides.block_tree > 0 ? std::min(overrides.block_tree, remaining) : remaining / 8;
        remaining -= std::min(block_tree_db, remaining);

        if (overrides.coins_db > 0) {
            coins_db = std::min(overrides.coins_db, remaining);
            remaining -= coins_db;
        } else {
            coins_db = std::min(remaining / 4, remaining);
            remaining -= std::min(coins_db, remaining);
        }

        if (overrides.coins > 0) {
            coins = std::min(overrides.coins, remaining);
        } else {
            coins = remaining;
        }
    }
};
} // namespace kernel

#endif // BITCOIN_KERNEL_CACHES_H
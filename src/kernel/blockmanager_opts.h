// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_BLOCKMANAGER_OPTS_H
#define BITCOIN_KERNEL_BLOCKMANAGER_OPTS_H

#include <dbwrapper.h>
#include <kernel/notifications_interface.h>
#include <util/fs.h>

#include <cstdint>

class CChainParams;

namespace kernel {

static constexpr bool DEFAULT_XOR_BLOCKSDIR{true};

static constexpr bool DEFAULT_BLOCK_ZSTD{true};
static constexpr int DEFAULT_BLOCK_ZSTD_LEVEL{20};
static constexpr bool DEFAULT_BLOCK_ZSTD_DECOMPRESS{true};

/**
 * An options struct for `BlockManager`, more ergonomically referred to as
 * `BlockManager::Options` due to the using-declaration in `BlockManager`.
 */
struct BlockManagerOpts {
    const CChainParams& chainparams;
    bool use_xor{DEFAULT_XOR_BLOCKSDIR};
    uint64_t prune_target{0};
    int64_t prune_target_during_init{-1};
    bool fast_prune{false};
    const fs::path blocks_dir;
    Notifications& notifications;
    DBParams block_tree_db_params;
    bool block_zstd{DEFAULT_BLOCK_ZSTD};
    int block_zstd_level{DEFAULT_BLOCK_ZSTD_LEVEL};
    bool block_zstd_decompress{DEFAULT_BLOCK_ZSTD_DECOMPRESS};
    fs::path block_zstd_dict{};
};

} // namespace kernel

#endif // BITCOIN_KERNEL_BLOCKMANAGER_OPTS_H

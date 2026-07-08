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

//! -blockdecompresspar default (0 = auto from -par, 1 = serial decompress only).
static constexpr int DEFAULT_BLOCK_DECOMPRESS_PAR{0};
static constexpr int MAX_BLOCK_DECOMPRESS_PAR{8};
//! Minimum on-disk payload size before parallel zstd decompression is used.
static constexpr size_t BLOCK_DECOMPRESS_PARALLEL_MIN_SIZE{32 << 10};

//! -blockindexsync: 0 = nosync except ALWAYS/shutdown, 1 = always fsync, 2 = auto (nosync during IBD).
static constexpr int DEFAULT_BLOCK_INDEX_SYNC{2};

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
    //! Parallel block zstd decompress worker threads (0 = serial; >0 from -blockdecompresspar).
    int block_decompress_workers{0};
    //! Block index LMDB fsync policy (see DEFAULT_BLOCK_INDEX_SYNC).
    int block_index_sync{DEFAULT_BLOCK_INDEX_SYNC};
};

} // namespace kernel

#endif // BITCOIN_KERNEL_BLOCKMANAGER_OPTS_H

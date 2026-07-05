// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_BENCHSTATS_H
#define BITCOIN_UTIL_BENCHSTATS_H

#include <atomic>
#include <cstdint>

namespace util {

//! Thread-safe accumulators for IBD read-path instrumentation (-benchstats=1).
struct BenchStatsCounters {
    std::atomic<uint64_t> block_read_disk_us{0};
    std::atomic<uint64_t> block_decompress_us{0};
    std::atomic<uint64_t> block_decompress_jobs{0};
    std::atomic<uint64_t> block_prefetch_hit{0};
    std::atomic<uint64_t> block_prefetch_wait_us{0};
    std::atomic<uint64_t> coin_prefetch_prevouts{0};
    std::atomic<uint64_t> coin_prefetch_misses{0};
    std::atomic<uint64_t> coin_prefetch_lmdb_us{0};
    std::atomic<uint64_t> coin_prefetch_decode_us{0};
    std::atomic<uint64_t> coin_prefetch_warm_us{0};
    std::atomic<uint64_t> blocks_connected{0};
};

extern BenchStatsCounters g_benchstats;
extern std::atomic<bool> g_benchstats_enabled;

//! Emit counters when debug=bench and -benchstats=1 (every 1000 blocks or on shutdown).
void MaybeLogBenchStats(bool force = false);

} // namespace util

#endif // BITCOIN_UTIL_BENCHSTATS_H
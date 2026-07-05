// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/benchstats.h>

#include <logging.h>
#include <tinyformat.h>

namespace util {

BenchStatsCounters g_benchstats;
std::atomic<bool> g_benchstats_enabled{false};

namespace {

uint64_t Exchange(std::atomic<uint64_t>& counter)
{
    return counter.exchange(0, std::memory_order_relaxed);
}

} // namespace

void MaybeLogBenchStats(const bool force)
{
    if (!g_benchstats_enabled.load(std::memory_order_relaxed)) return;
    if (!force && !LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug)) return;

    const uint64_t blocks{Exchange(g_benchstats.blocks_connected)};
    if (!force && blocks == 0) return;
    if (!force && blocks % 1000 != 0) return;

    const uint64_t disk_us{Exchange(g_benchstats.block_read_disk_us)};
    const uint64_t decompress_us{Exchange(g_benchstats.block_decompress_us)};
    const uint64_t par_jobs{Exchange(g_benchstats.block_decompress_jobs)};
    const uint64_t prefetch_hit{Exchange(g_benchstats.block_prefetch_hit)};
    const uint64_t prefetch_wait_us{Exchange(g_benchstats.block_prefetch_wait_us)};
    const uint64_t coin_prevouts{Exchange(g_benchstats.coin_prefetch_prevouts)};
    const uint64_t coin_miss{Exchange(g_benchstats.coin_prefetch_misses)};
    const uint64_t coin_lmdb_us{Exchange(g_benchstats.coin_prefetch_lmdb_us)};
    const uint64_t coin_decode_us{Exchange(g_benchstats.coin_prefetch_decode_us)};
    const uint64_t coin_warm_us{Exchange(g_benchstats.coin_prefetch_warm_us)};

    LogPrintf("benchstats: block disk=%.1fms decompress=%.1fms par_jobs=%u prefetch_hit=%u prefetch_wait=%.1fms | "
              "coin prevouts=%u miss=%u lmdb=%.1fms decode=%.1fms warm=%.1fms | blocks=%u\n",
              disk_us / 1000.0, decompress_us / 1000.0, par_jobs, prefetch_hit, prefetch_wait_us / 1000.0,
              coin_prevouts, coin_miss, coin_lmdb_us / 1000.0, coin_decode_us / 1000.0, coin_warm_us / 1000.0,
              blocks);
}

} // namespace util
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

std::atomic<uint64_t> g_abc_last_connect_tip_us{0};

uint64_t SteadyNowUs()
{
    return static_cast<uint64_t>(Ticks<std::chrono::microseconds>(SteadyClock::now().time_since_epoch()));
}

} // namespace

void BenchStatsRecordConnectTipEnd()
{
    if (!g_benchstats_enabled.load(std::memory_order_relaxed)) return;
    g_abc_last_connect_tip_us.store(SteadyNowUs(), std::memory_order_relaxed);
}

void BenchStatsRecordAbcIdle()
{
    if (!g_benchstats_enabled.load(std::memory_order_relaxed)) return;
    const uint64_t last{g_abc_last_connect_tip_us.load(std::memory_order_relaxed)};
    if (last == 0) return;
    const uint64_t now{SteadyNowUs()};
    if (now > last) {
        BenchStatsAdd(g_benchstats.abc_idle_us, now - last);
        g_abc_last_connect_tip_us.store(now, std::memory_order_relaxed);
    }
}

void MaybeLogBenchStats(const bool force)
{
    if (!g_benchstats_enabled.load(std::memory_order_relaxed)) return;

    const uint64_t blocks{g_benchstats.blocks_connected.load(std::memory_order_relaxed)};
    if (!force && blocks == 0) return;
    if (!force && blocks % 1000 != 0) return;

    const uint64_t blocks_logged{Exchange(g_benchstats.blocks_connected)};

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
    const uint64_t skip_threshold{Exchange(g_benchstats.coin_prefetch_skip_threshold)};
    const uint64_t skip_workers{Exchange(g_benchstats.coin_prefetch_skip_workers)};
    const uint64_t readers_full{Exchange(g_benchstats.coin_prefetch_readers_full)};
    const uint64_t chainstate_flush_us{Exchange(g_benchstats.chainstate_flush_us)};
    const uint64_t block_index_flush_us{Exchange(g_benchstats.block_index_flush_us)};
    const uint64_t flush_encode_us{Exchange(g_benchstats.flush_encode_us)};
    const uint64_t flush_lmdb_us{Exchange(g_benchstats.flush_lmdb_us)};
    const uint64_t stale_retries{Exchange(g_benchstats.flush_stale_retries)};
    const uint64_t txindex_write_us{Exchange(g_benchstats.txindex_write_us)};
    const uint64_t block_index_sync{Exchange(g_benchstats.block_index_sync_writes)};
    const uint64_t parallel_flushes{Exchange(g_benchstats.parallel_lmdb_flushes)};
    const uint64_t connect_chainstate_us{Exchange(g_benchstats.connect_chainstate_us)};
    const uint64_t cs_main_hold_us{Exchange(g_benchstats.cs_main_hold_us)};
    const uint64_t flush_mutex_wait_us{Exchange(g_benchstats.flush_mutex_wait_us)};
    const uint64_t blkidx_mutex_wait_us{Exchange(g_benchstats.blkidx_mutex_wait_us)};
    const uint64_t abc_idle_us{Exchange(g_benchstats.abc_idle_us)};
    const uint64_t script_jobs{Exchange(g_benchstats.script_jobs)};
    const uint64_t script_done{Exchange(g_benchstats.script_done)};

    LogPrintf("benchstats: block disk=%.1fms decompress=%.1fms par_jobs=%u prefetch_hit=%u prefetch_wait=%.1fms | "
              "coin prevouts=%u miss=%u lmdb=%.1fms decode=%.1fms warm=%.1fms skip_thr=%u skip_wrk=%u readers_full=%u | "
              "flush chainstate=%.1fms block_index=%.1fms encode=%.1fms lmdb=%.1fms stale=%u parallel=%u txindex=%.1fms blkidx_sync=%u connect_cs=%.1fms | "
              "wait cs_main_hold=%.1fms flush_mutex_wait=%.1fms blkidx_mutex_wait=%.1fms abc_idle=%.1fms script_jobs=%u script_done=%u | "
              "blocks=%u\n",
              disk_us / 1000.0, decompress_us / 1000.0, par_jobs, prefetch_hit, prefetch_wait_us / 1000.0,
              coin_prevouts, coin_miss, coin_lmdb_us / 1000.0, coin_decode_us / 1000.0, coin_warm_us / 1000.0,
              skip_threshold, skip_workers, readers_full,
              chainstate_flush_us / 1000.0, block_index_flush_us / 1000.0, flush_encode_us / 1000.0, flush_lmdb_us / 1000.0,
              stale_retries, parallel_flushes, txindex_write_us / 1000.0, block_index_sync, connect_chainstate_us / 1000.0,
              cs_main_hold_us / 1000.0, flush_mutex_wait_us / 1000.0, blkidx_mutex_wait_us / 1000.0, abc_idle_us / 1000.0,
              script_jobs, script_done,
              blocks_logged);
}

} // namespace util
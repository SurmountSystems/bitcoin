// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_BENCHSTATS_H
#define BITCOIN_UTIL_BENCHSTATS_H

#include <atomic>
#include <chrono>
#include <cstdint>

#include <util/time.h>

namespace util {

//! Thread-safe accumulators for IBD instrumentation (-benchstats=1).
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
    std::atomic<uint64_t> coin_prefetch_skip_threshold{0};
    std::atomic<uint64_t> coin_prefetch_skip_workers{0};
    std::atomic<uint64_t> coin_prefetch_readers_full{0};
    std::atomic<uint64_t> chainstate_flush_us{0};
    std::atomic<uint64_t> block_index_flush_us{0};
    std::atomic<uint64_t> flush_encode_us{0};
    std::atomic<uint64_t> flush_lmdb_us{0};
    std::atomic<uint64_t> flush_stale_retries{0};
    std::atomic<uint64_t> txindex_write_us{0};
    std::atomic<uint64_t> block_index_sync_writes{0};
    std::atomic<uint64_t> parallel_lmdb_flushes{0};
    std::atomic<uint64_t> connect_chainstate_us{0};
    std::atomic<uint64_t> cs_main_hold_us{0};
    std::atomic<uint64_t> flush_mutex_wait_us{0};
    std::atomic<uint64_t> blkidx_mutex_wait_us{0};
    std::atomic<uint64_t> abc_idle_us{0};
    //! script_jobs: checks queued via CCheckQueue::Add; script_done: checks executed (not skipped).
    std::atomic<uint64_t> script_jobs{0};
    std::atomic<uint64_t> script_done{0};
    std::atomic<uint64_t> blocks_connected{0};
};

extern BenchStatsCounters g_benchstats;
extern std::atomic<bool> g_benchstats_enabled;

inline void BenchStatsAdd(std::atomic<uint64_t>& counter, const uint64_t delta)
{
    if (g_benchstats_enabled.load(std::memory_order_relaxed)) {
        counter.fetch_add(delta, std::memory_order_relaxed);
    }
}

inline void BenchStatsInc(std::atomic<uint64_t>& counter)
{
    BenchStatsAdd(counter, 1);
}

//! RAII accumulator for cs_main hold time (pauses while cs_main is intentionally released).
class BenchStatsCsMainHold
{
    SteadyClock::time_point m_start{};
    uint64_t m_accum_us{0};
    bool m_active{false};

public:
    BenchStatsCsMainHold() { Resume(); }
    void Pause()
    {
        if (!m_active || !g_benchstats_enabled.load(std::memory_order_relaxed)) return;
        m_accum_us += static_cast<uint64_t>(Ticks<std::chrono::microseconds>(SteadyClock::now() - m_start));
        m_active = false;
    }
    void Resume()
    {
        if (!g_benchstats_enabled.load(std::memory_order_relaxed)) return;
        if (!m_active) {
            m_start = SteadyClock::now();
            m_active = true;
        }
    }
    ~BenchStatsCsMainHold()
    {
        Pause();
        BenchStatsAdd(g_benchstats.cs_main_hold_us, m_accum_us);
    }
};

//! Record ConnectTip return and ABC idle gaps (ActivateBestChain, no block ready).
void BenchStatsRecordConnectTipEnd();
void BenchStatsRecordAbcIdle();

//! Emit counters when -benchstats=1 (every 1000 blocks or on shutdown; no debug=bench required).
void MaybeLogBenchStats(bool force = false);

} // namespace util

#endif // BITCOIN_UTIL_BENCHSTATS_H
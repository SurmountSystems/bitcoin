// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_DBCACHE_H
#define BITCOIN_NODE_DBCACHE_H

#include <util/byte_units.h>

#include <cstdint>
#include <limits>
#include <optional>

//! min. -dbcache (bytes)
static constexpr uint64_t MIN_DBCACHE_BYTES{4_MiB};
//! Automatic -dbcache floor (bytes)
static constexpr uint64_t MIN_DEFAULT_DBCACHE{100_MiB};
//! Automatic -dbcache cap (bytes) on 64-bit; 32-bit keeps a 2 GiB cap.
static constexpr uint64_t MAX_DEFAULT_DBCACHE{SIZE_MAX > UINT32_MAX ? 48_GiB : 2_GiB};
//! Assumed total RAM when we cannot determine it.
static constexpr uint64_t FALLBACK_RAM_BYTES{SIZE_MAX > UINT32_MAX ? 4_GiB : 2_GiB};
//! Default reserved non-dbcache memory usage (bytes).
static constexpr uint64_t DEFAULT_RESERVED_RAM{2_GiB};
//! Legacy alias for tests and callers that expect a compile-time constant.
static constexpr uint64_t RESERVED_RAM{DEFAULT_RESERVED_RAM};
//! Maximum dbcache size on current architecture.
static constexpr uint64_t MAX_DBCACHE_BYTES{SIZE_MAX > UINT32_MAX ? std::numeric_limits<uint64_t>::max() : 1_GiB};

namespace node {
size_t GetTotalRam() noexcept;
size_t GetReservedRamBytes(std::optional<uint64_t> reserved_ram_mib = {}) noexcept;
size_t GetUsableRam(uint64_t total_ram, uint64_t reserved_ram_bytes) noexcept;

//! Default total -dbcache during IBD (~62.5% of usable RAM).
size_t GetDefaultDbCacheIbd(std::optional<uint64_t> total_ram = {}, std::optional<uint64_t> reserved_ram_mib = {}) noexcept;
//! Default total -dbcache after IBD (~25% of usable RAM).
size_t GetDefaultDbCacheSynced(std::optional<uint64_t> total_ram = {}, std::optional<uint64_t> reserved_ram_mib = {}) noexcept;

//! Backward-compatible alias: returns the IBD default profile.
size_t GetDefaultDBCache(std::optional<uint64_t> total_ram = {}, std::optional<uint64_t> reserved_ram_mib = {}) noexcept;

bool ShouldWarnOversizedDbCache(uint64_t dbcache, uint64_t total_ram, std::optional<uint64_t> reserved_ram_mib = {}) noexcept;
} // namespace node

#endif // BITCOIN_NODE_DBCACHE_H
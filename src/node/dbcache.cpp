// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/dbcache.h>

#include <common/system_ram.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace node {
namespace {
constexpr uint64_t IBD_RAM_FRACTION_NUM{5};
constexpr uint64_t IBD_RAM_FRACTION_DEN{8};
constexpr uint64_t SYNCED_RAM_FRACTION_NUM{1};
constexpr uint64_t SYNCED_RAM_FRACTION_DEN{4};

uint64_t ClampDbCache(uint64_t bytes) noexcept
{
    return std::max<uint64_t>(MIN_DEFAULT_DBCACHE, std::min(bytes, std::min(MAX_DEFAULT_DBCACHE, MAX_DBCACHE_BYTES)));
}

uint64_t ComputeProfileDbCache(uint64_t usable_ram, uint64_t num, uint64_t den) noexcept
{
    if (den == 0) return MIN_DEFAULT_DBCACHE;
    return ClampDbCache(usable_ram * num / den);
}
} // namespace

size_t GetTotalRam() noexcept { return TryGetTotalRam().value_or(FALLBACK_RAM_BYTES); }

size_t GetReservedRamBytes(std::optional<uint64_t> reserved_ram_mib) noexcept
{
    if (reserved_ram_mib && *reserved_ram_mib > 0) {
        return *reserved_ram_mib * 1_MiB;
    }
    return DEFAULT_RESERVED_RAM;
}

size_t GetUsableRam(uint64_t total_ram, uint64_t reserved_ram_bytes) noexcept
{
    return total_ram > reserved_ram_bytes ? total_ram - reserved_ram_bytes : 0;
}

size_t GetDefaultDbCacheIbd(std::optional<uint64_t> total_ram, std::optional<uint64_t> reserved_ram_mib) noexcept
{
    if (!total_ram) total_ram.emplace(GetTotalRam());
    const uint64_t reserved{GetReservedRamBytes(reserved_ram_mib)};
    const uint64_t usable{GetUsableRam(*total_ram, reserved)};
    return ComputeProfileDbCache(usable, IBD_RAM_FRACTION_NUM, IBD_RAM_FRACTION_DEN);
}

size_t GetDefaultDbCacheSynced(std::optional<uint64_t> total_ram, std::optional<uint64_t> reserved_ram_mib) noexcept
{
    if (!total_ram) total_ram.emplace(GetTotalRam());
    const uint64_t reserved{GetReservedRamBytes(reserved_ram_mib)};
    const uint64_t usable{GetUsableRam(*total_ram, reserved)};
    return ComputeProfileDbCache(usable, SYNCED_RAM_FRACTION_NUM, SYNCED_RAM_FRACTION_DEN);
}

size_t GetDefaultDBCache(std::optional<uint64_t> total_ram, std::optional<uint64_t> reserved_ram_mib) noexcept
{
    return GetDefaultDbCacheIbd(total_ram, reserved_ram_mib);
}

bool ShouldWarnOversizedDbCache(uint64_t dbcache, uint64_t total_ram, std::optional<uint64_t> reserved_ram_mib) noexcept
{
    const uint64_t reserved{GetReservedRamBytes(reserved_ram_mib)};
    const uint64_t headroom{GetUsableRam(total_ram, reserved)};
    return dbcache > std::max<uint64_t>(GetDefaultDBCache(total_ram, reserved_ram_mib), (headroom / 4) * 3);
}
} // namespace node
/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ZSwapStats {
    std::uint64_t stored_pages {}; // total logical pages in pool
    std::uint64_t pool_bytes {}; // actual compressed bytes used
    std::uint64_t written_back {}; // pages evicted to disk (cumulative)
    std::uint64_t pool_limit_hit {}; // failed because pool was full
    std::uint64_t reject_reclaim_fail {}; // failed to reclaim (pressure)
    std::string compressor {}; // e.g. "zstd"
    std::string zpool {}; // e.g. "zsmalloc" (may be empty in 6.18+)
    std::uint8_t max_pool_percent {}; // configured pool limit
    bool debugfs_available {}; // false if not root
};

struct SwapEntry {
    std::string type;
    std::string path;
    std::uint64_t size = 0;
    std::uint64_t used = 0;
    bool is_zswap      = false;
    std::optional<ZSwapStats> zswap_stats {}; // only set when is_zswap = true
};

struct MemInfo {
    std::uint64_t total     = 0;
    std::uint64_t used      = 0;
    std::uint64_t available = 0;
};

struct DiskInfo {
    std::uint64_t total     = 0;
    std::uint64_t used      = 0;
    std::uint64_t free      = 0;
    std::uint64_t available = 0;
};

class SystemInfo {
public:
    static std::string get_model_name() noexcept;
    static std::string get_cpu_cores_freq() noexcept;
    static std::string get_cpu_cache() noexcept;
    static bool has_aes() noexcept;
    static bool has_vmx() noexcept;
    static std::string get_virtualization() noexcept;
    static std::string get_os() noexcept;
    static std::string get_arch() noexcept;
    static std::string get_raw_arch() noexcept;
    static std::string get_kernel() noexcept;
    static std::string get_tcp_cc() noexcept;
    static std::string get_uptime() noexcept;
    static std::string get_load_avg() noexcept;

    static std::vector<SwapEntry> get_swaps() noexcept;

    static MemInfo get_memory_status() noexcept;
    static DiskInfo get_disk_usage(const std::string& mountpoint) noexcept;
    static std::string get_device_name(const std::string& path) noexcept;
};
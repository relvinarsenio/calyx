/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SwapEntry {
    std::string type;  // Partition, File, ZRAM, ZSwap
    std::string path;  // /dev/sda2, /swapfile, dll
    uint64_t size = 0;     // bytes
    uint64_t used = 0;     // bytes
    bool is_zswap = false;
};

struct MemInfo {
    uint64_t total;
    uint64_t used;
    uint64_t available;
};

struct DiskInfo {
    uint64_t total;
    uint64_t used;
    uint64_t free;
    uint64_t available;
};

class SystemInfo {
   public:
    static std::string get_model_name();
    static std::string get_cpu_cores_freq();
    static std::string get_cpu_cache();
    static bool has_aes();
    static bool has_vmx();
    static std::string get_virtualization();
    static std::string get_os();
    static std::string get_arch();
    static std::string get_raw_arch();
    static std::string get_kernel();
    static std::string get_tcp_cc();
    static std::string get_uptime();
    static std::string get_load_avg();

    static std::vector<SwapEntry> get_swaps();

    static MemInfo get_memory_status();
    static DiskInfo get_disk_usage(const std::string& mountpoint);
    static std::string get_device_name(const std::string& path);
};
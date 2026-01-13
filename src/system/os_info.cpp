/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/system_info.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <string>
#include <system_error>
#include <cstdlib>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#include <sys/sysinfo.h>
#include <sys/utsname.h>

namespace fs = std::filesystem;

std::string SystemInfo::get_virtualization() {
    std::error_code ec;
    if (fs::exists("/.dockerenv", ec) || fs::exists("/run/.containerenv", ec))
        return "Docker";

    if (fs::exists("/proc/1/environ", ec)) {
        std::ifstream f("/proc/1/environ");
        std::string env;
        while (std::getline(f, env, '\0')) {
            if (env.find("container=lxc") != std::string::npos)
                return "LXC";
            if (env.find("WSL_DISTRO_NAME=") != std::string::npos ||
                env.find("WSL_INTEROP=") != std::string::npos ||
                env.find("WSLENV=") != std::string::npos) {
                return "WSL";
            }
        }
    }

    if (fs::exists("/proc/user_beancounters"))
        return "OpenVZ";

    std::string release = get_kernel();
    if (release.find("Microsoft") != std::string::npos ||
        release.find("WSL") != std::string::npos) {
        return "WSL";
    }

    if (fs::exists("/dev/dxg") || fs::exists("/dev/lxss") || fs::exists("/usr/lib/wsl") ||
        fs::exists("/mnt/wsl")) {
        return "WSL";
    }

    bool hv_bit = false;
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);
    if (ecx & (1U << 31))
        hv_bit = true;
#endif

    if (hv_bit) {
#if defined(__x86_64__) || defined(__i386__)
        unsigned int leaf = 0x40000000;
        __cpuid(leaf, eax, ebx, ecx, edx);

        std::array<char, 13> vendor{};
        std::memcpy(vendor.data(), &ebx, 4);
        std::memcpy(vendor.data() + 4, &ecx, 4);
        std::memcpy(vendor.data() + 8, &edx, 4);
        vendor[12] = '\0';

        std::string sig(vendor.data());

        if (sig == "KVMKVMKVM")
            return "KVM";
        if (sig == "Microsoft Hv")
            return "Hyper-V";
        if (sig == "VMwareVMware")
            return "VMware";
        if (sig == "XenVMMXenVMM")
            return "Xen";
        if (sig == "VBoxVBoxVBox")
            return "VirtualBox";
        if (sig == "prl hyperv  ")
            return "Parallels";
        if (sig == "TCGTCGTCGTCG")
            return "QEMU";
#endif
    }

    std::ifstream dmi("/sys/class/dmi/id/product_name");
    std::string product;
    if (std::getline(dmi, product)) {
        if (product.find("KVM") != std::string::npos)
            return "KVM";
        if (product.find("QEMU") != std::string::npos)
            return "QEMU";
        if (product.find("VirtualBox") != std::string::npos)
            return "VirtualBox";
    }

    return hv_bit ? "Dedicated (Virtual)" : "Dedicated";
}

std::string SystemInfo::get_os() {
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("PRETTY_NAME=")) {
            auto val = line.substr(12);

            if (!val.empty() && (val.front() == '"' || val.front() == '\'')) {
                val = val.substr(1);
            }

            if (!val.empty() && (val.back() == '"' || val.back() == '\'')) {
                val.pop_back();
            }

            return val;
        }
    }
    return "Linux";
}

std::string SystemInfo::get_raw_arch() {
    struct utsname buffer;
    if (uname(&buffer) == 0) {
        return buffer.machine;
    }
    return "unknown";
}

std::string SystemInfo::get_arch() {
    std::string arch = get_raw_arch();
    if (arch == "unknown")
        return "Unknown";

    int bits = static_cast<int>(sizeof(void*) * 8);
    if (arch.find("64") != std::string::npos || arch == "s390x") {
        bits = 64;
    } else if (arch.find("86") != std::string::npos || arch.starts_with("arm")) {
        bits = 32;
    }
    return std::format("{} ({} Bit)", arch, bits);
}

std::string SystemInfo::get_kernel() {
    struct utsname buffer;
    if (uname(&buffer) == 0)
        return buffer.release;
    return "Unknown";
}

std::string SystemInfo::get_tcp_cc() {
    std::ifstream f("/proc/sys/net/ipv4/tcp_congestion_control");
    std::string s;
    if (f >> s)
        return s;
    return "Unknown";
}

std::string SystemInfo::get_uptime() {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        auto up = si.uptime;
        int days = static_cast<int>(up / 86400);
        int hours = static_cast<int>((up % 86400) / 3600);
        int mins = static_cast<int>((up % 3600) / 60);
        std::string uptime_str;
        if (days)
            uptime_str += std::format("{} {}, ", days, days == 1 ? "day" : "days");
        if (days || hours)
            uptime_str += std::format("{} {}, ", hours, hours == 1 ? "hour" : "hours");
        uptime_str += std::format("{} {}", mins, mins == 1 ? "min" : "mins");

        return uptime_str;
    }
    return "Unknown";
}

std::string SystemInfo::get_load_avg() {
    double loads[3];
    if (getloadavg(loads, 3) != -1) {
        return std::format("{:.2f}, {:.2f}, {:.2f}", loads[0], loads[1], loads[2]);
    }
    return "Unknown";
}
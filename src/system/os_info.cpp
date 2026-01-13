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

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <stdlib.h>

namespace fs = std::filesystem;

std::string SystemInfo::get_virtualization() {
    if (fs::exists("/.dockerenv") || fs::exists("/run/.containerenv"))
        return "Docker";

    if (fs::exists("/proc/1/environ")) {
        std::ifstream f("/proc/1/environ");
        std::string env;
        while (std::getline(f, env, '\0')) {
            if (env.find("container=lxc") != std::string::npos)
                return "LXC";
        }
    }

    if (fs::exists("/proc/user_beancounters"))
        return "OpenVZ";

    struct utsname buffer;
    if (uname(&buffer) == 0) {
        std::string release = buffer.release;
        if (release.find("Microsoft") != std::string::npos ||
            release.find("WSL") != std::string::npos)
            return "WSL";
    }

    if (fs::exists("/proc/1/environ")) {
        std::ifstream f("/proc/1/environ");
        std::string env;
        while (std::getline(f, env, '\0')) {
            if (env.find("WSL_DISTRO_NAME=") != std::string::npos ||
                env.find("WSL_INTEROP=") != std::string::npos ||
                env.find("WSLENV=") != std::string::npos) {
                return "WSL";
            }
        }
    }

    if (fs::exists("/dev/dxg")) {
        return "WSL";
    }
    if (fs::exists("/dev/lxss")) {
        return "WSL";
    }

    if (fs::exists("/usr/lib/wsl") || fs::exists("/mnt/wsl")) {
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
            if (val.size() >= 2 && val.front() == '"')
                val = val.substr(1, val.size() - 2);
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
        return std::format("{} days, {} hour {} min", days, hours, mins);
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
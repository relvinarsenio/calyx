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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <string>
#include <system_error>

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
        std::ifstream env_file("/proc/1/environ");
        if (env_file) {
            std::string env_line;
            while (std::getline(env_file, env_line, '\0')) {
                if (env_line.find("container=lxc") != std::string::npos)
                    return "LXC";
                if (env_line.find("WSL_DISTRO_NAME=") != std::string::npos ||
                    env_line.find("WSL_INTEROP=") != std::string::npos ||
                    env_line.find("WSLENV=") != std::string::npos) {
                    return "WSL";
                }
            }
        }
    }

    if (fs::exists("/proc/user_beancounters", ec))
        return "OpenVZ";

    std::string release = get_kernel();
    if (release.find("Microsoft") != std::string::npos ||
        release.find("WSL") != std::string::npos) {
        return "WSL";
    }

    if (fs::exists("/dev/dxg", ec) || fs::exists("/dev/lxss", ec) ||
        fs::exists("/usr/lib/wsl", ec) || fs::exists("/mnt/wsl", ec)) {
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

        std::array<char, 13> hv_vendor{};
        std::memcpy(hv_vendor.data(), &ebx, 4);
        std::memcpy(hv_vendor.data() + 4, &ecx, 4);
        std::memcpy(hv_vendor.data() + 8, &edx, 4);
        hv_vendor[12] = '\0';

        std::string hv_sig(hv_vendor.data());

        if (hv_sig == "KVMKVMKVM")
            return "KVM";
        if (hv_sig == "Microsoft Hv")
            return "Hyper-V";
        if (hv_sig == "VMwareVMware")
            return "VMware";
        if (hv_sig == "XenVMMXenVMM")
            return "Xen";
        if (hv_sig == "VBoxVBoxVBox")
            return "VirtualBox";
        if (hv_sig == "prl hyperv  ")
            return "Parallels";
        if (hv_sig == "TCGTCGTCGTCG")
            return "QEMU";
#endif
    }

    std::ifstream dmi_file("/sys/class/dmi/id/product_name");
    if (dmi_file) {
        std::string product_name;
        if (std::getline(dmi_file, product_name)) {
            if (product_name.find("KVM") != std::string::npos)
                return "KVM";
            if (product_name.find("QEMU") != std::string::npos)
                return "QEMU";
            if (product_name.find("VirtualBox") != std::string::npos)
                return "VirtualBox";
        }
    }

    return hv_bit ? "Dedicated (Virtual)" : "Dedicated";
}

std::string SystemInfo::get_os() {
    std::ifstream os_file("/etc/os-release");
    if (os_file) {
        std::string line;
        while (std::getline(os_file, line)) {
            if (line.starts_with("PRETTY_NAME=")) {
                auto pretty_name = line.substr(12);

                if (!pretty_name.empty() &&
                    (pretty_name.front() == '"' || pretty_name.front() == '\'')) {
                    pretty_name = pretty_name.substr(1);
                }

                if (!pretty_name.empty() &&
                    (pretty_name.back() == '"' || pretty_name.back() == '\'')) {
                    pretty_name.pop_back();
                }

                if (pretty_name.empty()) {
                    return "Linux";
                }

                return pretty_name;
            }
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
    std::ifstream tcp_file("/proc/sys/net/ipv4/tcp_congestion_control");
    if (tcp_file) {
        std::string cc_algo;
        if (tcp_file >> cc_algo)
            return cc_algo;
    }
    return "Unknown";
}

std::string SystemInfo::get_uptime() {
    struct sysinfo sys_info;
    if (sysinfo(&sys_info) == 0) {
        auto uptime_sec = sys_info.uptime;
        int days = static_cast<int>(uptime_sec / 86400);
        int hours = static_cast<int>((uptime_sec % 86400) / 3600);
        int mins = static_cast<int>((uptime_sec % 3600) / 60);
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
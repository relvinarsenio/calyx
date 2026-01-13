/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/system_info.hpp"
#include "include/utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#include <sys/utsname.h>
#include <unistd.h>

namespace {
#if !defined(__i386__) && !defined(__x86_64__)
static std::string cached_cpuinfo;
static std::mutex cpuinfo_mutex;
static bool cpuinfo_loaded = false;

const std::string& get_cached_cpuinfo() {
    std::lock_guard<std::mutex> lock(cpuinfo_mutex);
    if (!cpuinfo_loaded) {
        std::ifstream f("/proc/cpuinfo");
        if (f.is_open()) {
            std::ostringstream oss;
            oss << f.rdbuf();
            cached_cpuinfo = oss.str();
        }
        cpuinfo_loaded = true;
    }
    return cached_cpuinfo;
}
#endif

bool is_starts_with_ic(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size())
        return false;
    return std::equal(prefix.begin(), prefix.end(), str.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

#if !defined(__i386__) && !defined(__x86_64__)
bool cpu_has_flag(std::string_view flag) {
    const auto& cpuinfo = get_cached_cpuinfo();
    if (cpuinfo.empty())
        return false;

    size_t field_pos = cpuinfo.find("\nflags");
    if (field_pos == std::string::npos) {
        field_pos = cpuinfo.find("\nFeatures");
    }

    if (field_pos == std::string::npos) {
        if (cpuinfo.starts_with("flags"))
            field_pos = 0;
        else if (cpuinfo.starts_with("Features"))
            field_pos = 0;
    } else {
        ++field_pos;
    }

    if (field_pos == std::string::npos)
        return false;

    size_t line_end = cpuinfo.find('\n', field_pos);
    if (line_end == std::string::npos)
        line_end = cpuinfo.length();

    std::string_view flags_line(cpuinfo.data() + field_pos, line_end - field_pos);

    size_t colon_pos = flags_line.find(':');
    if (colon_pos != std::string_view::npos) {
        flags_line = flags_line.substr(colon_pos + 1);
    }

    size_t pos = 0;
    while (pos < flags_line.size()) {
        while (pos < flags_line.size() &&
               std::isspace(static_cast<unsigned char>(flags_line[pos]))) {
            ++pos;
        }
        if (pos >= flags_line.size())
            break;

        size_t token_start = pos;
        while (pos < flags_line.size() &&
               !std::isspace(static_cast<unsigned char>(flags_line[pos]))) {
            ++pos;
        }

        std::string_view token = flags_line.substr(token_start, pos - token_start);
        if (token == flag) {
            return true;
        }
    }

    return false;
}
#endif

}  // namespace

std::string SystemInfo::get_model_name() {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext >= 0x80000004) {
        std::array<unsigned int, 12> data{};
        __cpuid(0x80000002, data[0], data[1], data[2], data[3]);
        __cpuid(0x80000003, data[4], data[5], data[6], data[7]);
        __cpuid(0x80000004, data[8], data[9], data[10], data[11]);

        std::string brand;
        brand.resize(48);
        std::memcpy(brand.data(), data.data(), std::min(brand.size(), sizeof(data)));

        if (auto pos = brand.find('\0'); pos != std::string::npos) {
            brand.resize(pos);
        }

        brand = trim(brand);
        if (!brand.empty())
            return brand;
    }
#endif

    std::ifstream f("/proc/cpuinfo");
    std::string line;
    const std::array<std::string, 5> keys = {"model name", "hardware", "processor", "cpu", "Model"};

    while (std::getline(f, line)) {
        for (const auto& k : keys) {
            if (is_starts_with_ic(line, k)) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string_view val = trim_sv(std::string_view(line).substr(colon + 1));
                    if (!val.empty())
                        return std::string(val);
                }
            }
        }
    }

    std::ifstream dt("/sys/firmware/devicetree/base/model");
    if (dt) {
        std::string model;
        if (std::getline(dt, model)) {
            model = trim(model);
            if (!model.empty())
                return model;
        }
    }

    std::string arch = SystemInfo::get_raw_arch();
    if (arch != "unknown") {
        return arch;
    }
    return "Unknown CPU";
}

std::string SystemInfo::get_cpu_cores_freq() {
    long cores = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    double freq_mhz = 0.0;

    std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");

    std::string line;
    if (f >> line) {
        uint64_t val = 0;
        auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), val);
        if (ec == std::errc()) {
            freq_mhz = static_cast<double>(val) / 1000.0;
        }
    }

    if (freq_mhz == 0.0) {
        std::ifstream cpuinfo("/proc/cpuinfo");
        while (std::getline(cpuinfo, line)) {
            if (line.starts_with("cpu MHz")) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string_view val_str = trim_sv(std::string_view(line).substr(colon + 1));
                    auto [ptr, ec] =
                        std::from_chars(val_str.data(), val_str.data() + val_str.size(), freq_mhz);
                    if (ec == std::errc()) {
                        break;
                    }
                }
            }
        }
    }
    return std::format("{} @ {:.1f} MHz", cores, freq_mhz);
}

std::string SystemInfo::get_cpu_cache() {
    auto parse_cache = [](std::string_view s) -> std::string {
        std::string_view sv = trim_sv(s);
        if (sv.empty())
            return "Unknown";

        uint64_t size = 0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), size);

        if (ec != std::errc())
            return std::string(sv);

        if (ptr < sv.data() + sv.size()) {
            char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(*ptr)));
            if (suffix == 'K')
                size *= 1024;
            else if (suffix == 'M')
                size *= 1024 * 1024;
        } else {
            if (static_cast<char>(std::toupper(static_cast<unsigned char>(sv.back()))) == 'K')
                size *= 1024;
            else if (std::isdigit(static_cast<unsigned char>(sv.back())))
                size *= 1024;
        }

        if (size >= 1024 * 1024)
            return std::format("{:.0f} MB", static_cast<double>(size) / (1024.0 * 1024.0));
        if (size >= 1024)
            return std::format("{:.0f} KB", static_cast<double>(size) / 1024.0);
        return std::format("{} B", size);
    };

    constexpr std::array<std::string_view, 4> caches = {"3", "2", "1", "0"};
    for (const auto& idx : caches) {
        std::string path = std::format("/sys/devices/system/cpu/cpu0/cache/index{}/size", idx);
        std::ifstream f(path);
        std::string size;
        if (f >> size)
            return parse_cache(size);
    }
    return "Unknown";
}

bool SystemInfo::has_aes() {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);
    return (ecx & (1U << 25)) != 0;  // AES-NI: ECX bit 25
#else
    return cpu_has_flag("aes");
#endif
}

bool SystemInfo::has_vmx() {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);

    // Intel VMX: ECX bit 5
    bool intel_vmx = (ecx & (1U << 5)) != 0;

    // AMD SVM: CPUID leaf 0x80000001, ECX bit 2
    bool amd_svm = false;
    unsigned int max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext >= 0x80000001) {
        __cpuid(0x80000001, eax, ebx, ecx, edx);
        amd_svm = (ecx & (1U << 2)) != 0;
    }

    return intel_vmx || amd_svm;
#else
    return cpu_has_flag("vmx") || cpu_has_flag("svm");
#endif
}
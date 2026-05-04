/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "system_info.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <numeric>
#include <span>
#include <spanstream>
#include <string>
#include <string_view>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#include "arm_info.hpp"

#include <ranges>
#include <sys/utsname.h>
#include <unistd.h>

namespace {

using namespace arm;

const std::string kCpuModel = []() -> std::string {
#if defined(__i386__) || defined(__x86_64__)
    std::uint32_t max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext >= 0x80000004) {
        std::array<std::uint32_t, 12> data {};
        __cpuid(0x80000002, data[0], data[1], data[2], data[3]);
        __cpuid(0x80000003, data[4], data[5], data[6], data[7]);
        __cpuid(0x80000004, data[8], data[9], data[10], data[11]);

        auto chars    = std::bit_cast<std::array<char, 48>>(data);
        auto null_it  = std::ranges::find(chars, '\0');
        auto brand_sv = trim_sv(std::string_view(chars.data(), toSize(std::distance(chars.begin(), null_it))));
        if (!brand_sv.empty()) return std::string(brand_sv);
    }
#endif

    auto dt_model = []() -> std::optional<std::string> {
        const std::string_view content = probe::kDtModelProbe;
        if (content.empty()) return std::nullopt;
        return std::string(content);
    }();

    if (dt_model && !dt_model->empty()) return *dt_model;

#if !defined(__i386__) && !defined(__x86_64__)
    if (auto arm_name = resolve_arm_model_name()) return *arm_name;
#endif

    const auto& cpuinfo                              = probe::kCpuInfoProbe;
    constexpr std::array<std::string_view, 5z> kKeys = { "model name", "model", "hardware", "cpu", "processor" };

    auto lines
        = std::views::split(cpuinfo, '\n') | std::views::transform([](auto raw) { return std::string_view(raw); });

    auto matches = std::views::cartesian_product(kKeys, lines) | std::views::filter([](auto pair) {
        auto [key, line] = pair;
        auto colon       = line.find(':');
        if (colon == std::string_view::npos) return false;
        auto tag = trim_sv(line.substr(0, colon));
        return tag.size() == key.size() && is_starts_with_ic(tag, key);
    }) | std::views::transform([](auto pair) {
        auto [key, line] = pair;
        auto colon       = line.find(':');
        return trim_sv(line.substr(colon + 1));
    }) | std::views::filter([](auto val) { return !val.empty(); })
        | std::views::take(1);

    if (auto it = matches.begin(); it != matches.end()) { return std::string(*it); }

    std::string arch = SystemInfo::get_raw_arch();
    return (arch != "unknown") ? arch : "Unknown CPU";
}();

const bool kHasAes = []() noexcept {
#if defined(__i386__) || defined(__x86_64__)
    std::uint32_t eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    return (ecx & (1u << 25)) != 0;
#else
    return cpu_has_flag("aes");
#endif
}();

const bool kHasVmx = []() noexcept {
#if defined(__i386__) || defined(__x86_64__)
    std::uint32_t eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;

    bool intel_vmx = (ecx & (1u << 5)) != 0;

    bool amd_svm          = false;
    std::uint32_t max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext >= 0x80000001) {
        __cpuid(0x80000001, eax, ebx, ecx, edx);
        amd_svm = (ecx & (1u << 2)) != 0;
    }

    return intel_vmx || amd_svm;
#else
    return cpu_has_flag("vmx") || cpu_has_flag("svm") || cpu_has_flag("virt");
#endif
}();

} // namespace

/**
 * @brief Retrieves the human-readable CPU model name.
 *
 * @details
 * Resolver priority:
 * 1. x86: Uses __cpuid (brand string).
 * 2. ARM: Uses Device Tree model name (/sys/firmware/devicetree/base/model).
 * 3. ARM: Uses hardware IDs (MIDR_EL1) and a comprehensive lookup table.
 * 4. Generic: Scans /proc/cpuinfo for specific priority keys.
 *
 * @return std::string The CPU model name or fallback value.
 */
std::string SystemInfo::get_model_name() noexcept {
    return kCpuModel;
}

std::string SystemInfo::get_cpu_cores_freq() noexcept {
    const std::int64_t cores           = std::max(1L, ::sysconf(_SC_NPROCESSORS_ONLN));
    const auto& [max_khz, is_true_max] = probe::kMaxFreqProbe;

    if (max_khz > 0) {
        const double mhz = toDouble(max_khz) / 1000.0;
        return is_true_max ? std::format("{} @ {:.1f} MHz (Max)", format_count(toSize(cores)), mhz)
                           : std::format("{} @ {:.1f} MHz", format_count(toSize(cores)), mhz);
    }
    return std::format("{} Cores", format_count(toSize(cores)));
}

std::string SystemInfo::get_cpu_cache() noexcept {
    return probe::kCpuCacheProbe;
}

bool SystemInfo::has_aes() noexcept {
    return kHasAes;
}

bool SystemInfo::has_vmx() noexcept {
    return kHasVmx;
}
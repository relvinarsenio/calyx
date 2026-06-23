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
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#include "arm_info.hpp"

#include <ranges>

namespace {

using namespace arm;

[[nodiscard]] std::string probe_cpu_model() {
#if defined(__i386__) || defined(__x86_64__)
    std::uint32_t max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext >= 0x80000004) {
        std::array<std::uint32_t, 12> data {};
        __cpuid(0x80000002, data[0], data[1], data[2], data[3]);
        __cpuid(0x80000003, data[4], data[5], data[6], data[7]);
        __cpuid(0x80000004, data[8], data[9], data[10], data[11]);

        const auto chars    = std::bit_cast<std::array<char, 48>>(data);
        const auto null_it  = std::ranges::find(chars, '\0');
        const auto brand_sv = trim_sv(std::string_view(chars.data(), toSize(std::distance(chars.begin(), null_it))));
        if (!brand_sv.empty()) { return std::string(brand_sv); }
    }
#endif

    if (!probe::get_dt_model_probe().empty()) { return probe::get_dt_model_probe(); }

#if !defined(__i386__) && !defined(__x86_64__)
    if (const auto arm_name = resolve_arm_model_name()) { return *arm_name; }
#endif

    const auto& cpuinfo                              = probe::get_cpu_info_probe();
    constexpr std::array<std::string_view, 5z> kKeys = { "model name", "model", "hardware", "cpu", "processor" };

    if (const auto match = lookup_info_field(cpuinfo, kKeys)) { return std::string(*match); }

    const std::string arch = SystemInfo::get_raw_arch();
    return (arch != "unknown") ? arch : "Unknown CPU";
}

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
    static const std::string cpu_model = probe_cpu_model();
    return cpu_model;
}

std::string SystemInfo::get_cpu_cores_freq() noexcept {
    const auto cores_res     = posix::expect_result<posix::error_style::posix>(::sysconf(_SC_NPROCESSORS_ONLN));
    const std::int64_t cores = (cores_res && *cores_res > 0) ? *cores_res : 1L;
    const auto& [max_khz, is_true_max] = probe::get_max_freq_probe();

    if (max_khz > 0) {
        const double mhz = toDouble(max_khz) / 1000.0;
        return is_true_max ? std::format("{} @ {:.1f} MHz (Max)", format_count(toSize(cores)), mhz)
                           : std::format("{} @ {:.1f} MHz", format_count(toSize(cores)), mhz);
    }
    return std::format("{} Cores", format_count(toSize(cores)));
}

std::string SystemInfo::get_cpu_cache() noexcept {
    return probe::get_cpu_cache_probe();
}

bool SystemInfo::has_aes() noexcept {
    return probe::get_cpu_features().has_aes;
}

bool SystemInfo::has_vmx() noexcept {
    return probe::get_cpu_features().has_vmx;
}
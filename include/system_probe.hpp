/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "posix.hpp"
#include "utils.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>

namespace probe {

/**
 * @brief Represents processor frequency details.
 */
struct FreqInfo {
    std::uint64_t khz { 0 };
    bool is_true_max { false };
};

/**
 * @brief Represents system architecture details.
 */
struct ArchInfo {
    std::string raw;
    std::string formatted;
};

struct CpuFeatures {
    bool has_aes    = false;
    bool has_vmx    = false;
    bool has_hv_bit = false;
};

/**
 * @brief Lazy accessors for system probes to prevent redundant startup I/O.
 */
const std::expected<::utsname, std::error_code>& get_uname_probe() noexcept;
const ArchInfo& get_arch_probe() noexcept;
const std::string& get_cpu_info_probe() noexcept;
const std::string& get_os_release_probe() noexcept;
const std::string& get_tcp_cc_probe() noexcept;
const std::string& get_cpu_cache_probe() noexcept;
const std::string& get_dt_model_probe() noexcept;
const std::string& get_midr_probe() noexcept;
bool get_zswap_enabled_probe() noexcept;
const FreqInfo& get_max_freq_probe() noexcept;
const std::string& get_virtualization_probe() noexcept;
const CpuFeatures& get_cpu_features() noexcept;

/**
 * @brief Checks if the CPU has a specific flag (for non-x86 architectures).
 */
bool cpu_has_flag(std::string_view flag) noexcept;

} // namespace probe

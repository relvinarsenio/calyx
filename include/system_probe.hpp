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

/**
 * @brief Cached system information probes.
 * @details Declared as extern const to prevent redundant dynamic initialization
 *          and startup I/O across multiple translation units, reducing compilation overhead.
 */
extern const std::expected<::utsname, std::error_code> kUnameProbe;
extern const ArchInfo kArchProbe;
extern const std::string kCpuInfoProbe;
extern const std::string kOsReleaseProbe;
extern const std::string kTcpCcProbe;
extern const std::string kCpuCacheProbe;
extern const std::string kDtModelProbe;
extern const std::string kMidrProbe;
extern const bool kZswapEnabledProbe;
extern const FreqInfo kMaxFreqProbe;
extern const std::string kVirtualizationProbe;

} // namespace probe

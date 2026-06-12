/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "posix.hpp"
#include "system_info.hpp"
#include "system_probe.hpp"
#include "utils.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

std::string SystemInfo::get_virtualization() noexcept {
    return probe::kVirtualizationProbe;
}

std::string SystemInfo::get_os() noexcept {
    static const std::string instance = []() -> std::string {
        static constexpr std::string_view kPrefix = "PRETTY_NAME=";
        const std::string_view content            = probe::kOsReleaseProbe;
        if (content.empty()) { return "Linux"; }

        auto pretty_name_lines = content | split_to_sv('\n')
            | std::views::filter([](auto line) { return line.starts_with(kPrefix) && line.size() > kPrefix.size(); })
            | std::views::transform([](auto line) { return unquote(line.substr(kPrefix.size())); })
            | std::views::filter([](auto unquoted) { return !unquoted.empty(); }) | std::views::take(1);

        auto first = pretty_name_lines.begin();
        return first != pretty_name_lines.end() ? std::string(*first) : "Linux";
    }();
    return instance;
}

std::string SystemInfo::get_raw_arch() noexcept {
    return probe::kArchProbe.raw;
}

std::string SystemInfo::get_arch() noexcept {
    return probe::kArchProbe.formatted;
}

std::string SystemInfo::get_kernel() noexcept {
    static const std::string instance = probe::kUnameProbe ? probe::kUnameProbe->release : std::string("Unknown");
    return instance;
}

std::string SystemInfo::get_tcp_cc() noexcept {
    const std::string& cc = probe::kTcpCcProbe;
    return cc.empty() ? std::string("Unknown") : cc;
}

std::string SystemInfo::get_uptime() noexcept {
    using namespace std::chrono;

    struct sysinfo sys_info {};
    if (!posix::expect_result<posix::error_style::posix>(::sysinfo(&sys_info))) { return "Unknown"; }

    auto total    = seconds(sys_info.uptime);
    auto up_days  = floor<days>(total);
    auto up_hours = floor<hours>(total - up_days);
    auto up_mins  = floor<minutes>(total - up_days - up_hours);

    struct TimeUnit {
        std::int64_t count;
        std::string_view singular;
        std::string_view plural;
    };
    const auto parts = std::array {
        TimeUnit { up_days.count(), "day", "days" },
        TimeUnit { up_hours.count(), "hour", "hours" },
    };

    const auto prefix = parts | std::views::filter([](const auto& time_unit) {
        return time_unit.count > 0;
    }) | std::views::transform([](const auto& time_unit) {
        return std::format("{} {}, ", time_unit.count, time_unit.count == 1 ? time_unit.singular : time_unit.plural);
    }) | std::views::join
        | std::ranges::to<std::string>();

    return prefix + std::format("{} {}", up_mins.count(), up_mins.count() == 1 ? "min" : "mins");
}

std::string SystemInfo::get_load_avg() noexcept {
    std::array<double, 3uz> loads {};
    return posix::expect_result<posix::error_style::posix>(::getloadavg(loads.data(), 3))
        .and_then([](int n_samples) noexcept -> std::expected<int, std::error_code> {
            return n_samples > 0 ? std::expected<int, std::error_code> { n_samples }
                                 : std::unexpected(std::make_error_code(std::errc::invalid_argument));
        })
        .transform([&loads](int n_samples) {
            constexpr std::string_view kDelimiter = ", ";
            return std::views::iota(0uz, 3uz)
                | std::views::transform([&loads, limit = toSize(n_samples)](std::size_t idx) {
                      return idx < limit ? std::format("{:.2f}", loads[idx]) : std::string("N/A");
                  })
                | std::views::join_with(kDelimiter) | std::ranges::to<std::string>();
        })
        .value_or("N/A");
}
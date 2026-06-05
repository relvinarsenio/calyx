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

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>
#include <vector>

namespace {

[[nodiscard]] std::string read_sysfs_param(const std::filesystem::path& p) {
    auto content = read_file(p);
    if (!content) { return {}; }
    return std::string(trim_sv(*content));
}

[[nodiscard]] std::optional<std::uint64_t> read_sysfs_u64(const std::filesystem::path& p) {
    auto content = read_file(p);
    if (!content) { return std::nullopt; }
    std::uint64_t val {};
    auto trimmed = trim_sv(*content);
    if (auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), val); ec == std::errc {}) {
        return val;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ZSwapStats> collect_zswap_stats() {
    ZSwapStats stats {};

    stats.compressor = read_sysfs_param("/sys/module/zswap/parameters/compressor");
    stats.zpool      = read_sysfs_param("/sys/module/zswap/parameters/zpool");

    if (auto pct = read_sysfs_param("/sys/module/zswap/parameters/max_pool_percent"); !pct.empty()) {
        std::uint8_t val {};
        if (auto [ptr, ec] = std::from_chars(pct.data(), pct.data() + pct.size(), val); ec == std::errc {}) {
            stats.max_pool_percent = val;
        }
    }

    auto stored = read_sysfs_u64("/sys/kernel/debug/zswap/stored_pages");
    if (!stored) {
        stats.debugfs_available = false;
        return stats;
    }

    stats.debugfs_available = true;
    stats.stored_pages      = *stored;

    if (auto v = read_sysfs_u64("/sys/kernel/debug/zswap/pool_total_size")) { stats.pool_bytes = *v; }
    if (auto v = read_sysfs_u64("/sys/kernel/debug/zswap/written_back_pages")) { stats.written_back = *v; }
    if (auto v = read_sysfs_u64("/sys/kernel/debug/zswap/pool_limit_hit")) { stats.pool_limit_hit = *v; }
    if (auto v = read_sysfs_u64("/sys/kernel/debug/zswap/reject_reclaim_fail")) { stats.reject_reclaim_fail = *v; }

    return stats;
}

} // namespace

MemInfo SystemInfo::get_memory_status() noexcept {
    const auto [initial_total, initial_available] = []() -> std::pair<std::uint64_t, std::uint64_t> {
        struct sysinfo si {};
        if (sysinfo(&si) == 0 && si.mem_unit > 0) {
            const std::uint64_t unit = si.mem_unit;
            return { safe_mul(si.totalram, unit).value_or(0), safe_mul(si.freeram, unit).value_or(0) };
        }
        return { 0, 0 };
    }();

    MemInfo info { .total = initial_total, .available = initial_available };

    info = parse_file_or(
        "/proc/meminfo",
        [&](std::string_view content) {
            auto mem_lines = content | split_to_sv('\n');

            auto extract_mem_value = [](std::string_view line) -> std::optional<std::uint64_t> {
                if (!line.starts_with("MemAvailable:")) { return std::nullopt; }
                auto value_part = trim_sv(line.substr(line.find(':') + 1));
                std::uint64_t kib_value {};
                auto [ptr, ec] = std::from_chars(value_part.data(), value_part.data() + value_part.size(), kib_value);
                return (ec == std::errc {}) ? std::optional<std::uint64_t> { kib_value } : std::nullopt;
            };

            auto available_mem = mem_lines | std::views::transform(extract_mem_value)
                | std::views::filter([](auto mem_result) { return mem_result.has_value(); })
                | std::views::transform([](auto mem_result) { return *mem_result; }) | std::views::take(1);

            if (auto first_value = available_mem.begin(); first_value != available_mem.end()
                && *first_value <= std::numeric_limits<std::uint64_t>::max() / 1024) {
                info.available = *first_value * 1024;
            }
            return info;
        },
        info);

    info.used = (info.total >= info.available) ? info.total - info.available : 0;

    return info;
}

DiskInfo SystemInfo::get_disk_usage(const std::string& mountpoint) noexcept {
    DiskInfo info {};
    const auto vfs = posix::statvfs(mountpoint.c_str());
    if (!vfs) { return info; }

    const std::uint64_t block_size = vfs->f_frsize;
    if (block_size == 0) { return info; }

    info.total     = safe_mul(vfs->f_blocks, block_size).value_or(0);
    info.free      = safe_mul(vfs->f_bfree, block_size).value_or(0);
    info.available = safe_mul(vfs->f_bavail, block_size).value_or(0);

    const auto used_blocks = (vfs->f_blocks > vfs->f_bfree) ? vfs->f_blocks - vfs->f_bfree : 0uz;
    info.used              = safe_mul(used_blocks, block_size).value_or(0);

    return info;
}

std::vector<SwapEntry> SystemInfo::get_swaps() noexcept {
    std::vector<SwapEntry> swaps;

    swaps = parse_file_or(
        "/proc/swaps",
        [](std::string_view content_sv) {
            auto parse_swap_line = [](std::string_view line) -> std::optional<SwapEntry> {
                constexpr std::size_t kFieldCount = 4;
                auto tokens                       = line | tokenize_sv() | std::views::take(kFieldCount);

                std::array<std::string_view, kFieldCount> swap_fields {};
                if (std::ranges::copy(tokens, swap_fields.begin()).out != swap_fields.end()) { return std::nullopt; }

                auto [path, type, size_str, used_str] = swap_fields;

                return SwapEntry { .type = path.contains("zram") ? "ZRAM" : capitalize(type),
                    .path                = std::string(path),
                    .size     = safe_mul(parse_number<std::uint64_t>(size_str).value_or(0), 1024ULL).value_or(0),
                    .used     = safe_mul(parse_number<std::uint64_t>(used_str).value_or(0), 1024ULL).value_or(0),
                    .is_zswap = false };
            };

            return content_sv | split_to_sv('\n') | std::views::drop(1) | std::views::transform(parse_swap_line)
                | std::views::filter([](const auto& swap_result) { return swap_result.has_value(); })
                | std::views::transform([](auto&& swap_result) { return std::move(*swap_result); })
                | std::ranges::to<std::vector>();
        },
        swaps);

    const auto zswap = []() -> std::optional<SwapEntry> {
        const bool enabled = probe::kZswapEnabledProbe;

        if (!enabled) { return std::nullopt; }

        auto zs = collect_zswap_stats();
        if (!zs) { return std::nullopt; }

        const std::uint64_t uncompressed = safe_mul(zs->stored_pages, get_page_size()).value_or(0);

        return SwapEntry { .type = "ZSwap",
            .path                = "Active",
            .size                = uncompressed,
            .used                = zs->pool_bytes,
            .is_zswap            = true,
            .zswap_stats         = std::move(zs) };
    }();

    if (zswap) { swaps.push_back(std::move(*zswap)); }

    return swaps;
}

std::string SystemInfo::get_device_name(const std::string& path) noexcept {
    auto file_stat = posix::stat(path);
    if (!file_stat) { return "unknown device"; }

    const std::string target_dev = std::format("{}:{}", major(file_stat->st_dev), minor(file_stat->st_dev));

    struct MatchResult {
        std::string_view exact_src;
        std::string_view exact_fs;
        std::string_view best_src;
        std::string_view best_fs;
        std::size_t best_len = 0;
    };

    return parse_file_or(
        "/proc/self/mountinfo",
        [&target_dev, &path](std::string_view content) {
            auto process_line = [&target_dev, &path](MatchResult&& result, std::string_view line) -> MatchResult {
                if (line.empty()) { return std::move(result); }

                constexpr std::size_t kMountFieldCount = 5;
                constexpr std::size_t kMountSepFields  = 2;

                std::array<std::string_view, kMountFieldCount> fixed_fields {};
                auto copy_res = std::ranges::copy(
                    line | tokenize_sv(' ') | std::views::take(kMountFieldCount), fixed_fields.begin());
                if (copy_res.out != fixed_fields.end()) { return std::move(result); }
                std::string_view major_minor = fixed_fields[2];
                std::string_view mount_point = fixed_fields[4];

                auto sep_pos = line.find(" - ");
                if (sep_pos == std::string_view::npos) { return std::move(result); }

                std::array<std::string_view, kMountSepFields> sep_fields {};
                auto sep_copy_res
                    = std::ranges::copy(line.substr(sep_pos + 3) | tokenize_sv(' ') | std::views::take(kMountSepFields),
                        sep_fields.begin());
                if (sep_copy_res.out != sep_fields.end()) { return std::move(result); }
                auto [fs_type, source] = sep_fields;

                if (major_minor == target_dev) {
                    result.exact_src = source;
                    result.exact_fs  = fs_type;
                }

                if (!path.starts_with(mount_point)) { return std::move(result); }

                const bool valid_boundary
                    = (path.size() == mount_point.size()) || (mount_point == "/") || (path[mount_point.size()] == '/');

                if (!valid_boundary || mount_point.size() <= result.best_len) { return std::move(result); }

                result.best_len = mount_point.size();
                result.best_src = source;
                result.best_fs  = fs_type;

                return std::move(result);
            };

            auto match = std::ranges::fold_left(content | split_to_sv('\n'), MatchResult {}, process_line);

            auto format_output = [](std::string_view src, std::string_view fs) -> std::string {
                if (src.empty()) { return {}; }
                if (src == fs) { return std::string(src); }
                return std::format("{} ({})", src, fs);
            };

            if (!match.exact_src.empty()) { return format_output(match.exact_src, match.exact_fs); }
            if (match.best_len > 0) { return format_output(match.best_src, match.best_fs); }
            return std::string("unknown device");
        },
        std::string { "unknown device" });
}

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
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::string read_sysfs_param(const std::filesystem::path& p) {
    auto content = read_file(p);
    if (!content) { return {}; }
    return trim(*content);
}

[[nodiscard]] std::optional<std::uint64_t> read_sysfs_u64(const std::filesystem::path& p) {
    auto content = read_file(p);
    if (!content) { return std::nullopt; }
    auto result = parse_number<std::uint64_t>(trim_sv(*content));
    return result ? std::optional(*result) : std::nullopt;
}

[[nodiscard]] std::optional<ZSwapStats> collect_zswap_stats() {
    ZSwapStats stats {};

    stats.compressor = read_sysfs_param("/sys/module/zswap/parameters/compressor");
    stats.zpool      = read_sysfs_param("/sys/module/zswap/parameters/zpool");

    if (auto pct = read_sysfs_param("/sys/module/zswap/parameters/max_pool_percent"); !pct.empty()) {
        if (auto val = parse_number<std::uint8_t>(pct)) { stats.max_pool_percent = *val; }
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

[[nodiscard]] std::optional<SwapEntry> parse_swap_line(std::string_view line) {
    constexpr std::size_t kFieldCount = 4;
    auto tokens                       = line | tokenize_sv() | std::views::take(kFieldCount);

    std::array<std::string_view, kFieldCount> swap_fields {};
    if (std::ranges::copy(tokens, swap_fields.begin()).out != swap_fields.end()) { return std::nullopt; }

    auto [path, type, size_str, used_str] = swap_fields;

    return SwapEntry { .type = path.contains("zram") ? "ZRAM" : capitalize(type),
        .path                = std::string(path),
        .size                = safe_mul(parse_number<std::uint64_t>(size_str).value_or(0), 1024ULL).value_or(0),
        .used                = safe_mul(parse_number<std::uint64_t>(used_str).value_or(0), 1024ULL).value_or(0),
        .is_zswap            = false };
}

struct MountMatch {
    std::string_view exact_src {};
    std::string_view exact_fs {};
    std::string_view best_src {};
    std::string_view best_fs {};
    std::size_t best_len = 0;
};

[[nodiscard]] MountMatch accumulate_mount_entry(
    MountMatch&& result, std::string_view line, std::string_view target_dev, std::string_view path) {
    if (line.empty()) { return std::move(result); }

    constexpr std::size_t kFixedFields = 5;
    constexpr std::size_t kSepFields   = 2;

    std::array<std::string_view, kFixedFields> fixed {};
    if (std::ranges::copy(line | tokenize_sv(' ') | std::views::take(kFixedFields), fixed.begin()).out != fixed.end()) {
        return std::move(result);
    }
    auto [mount_id, parent_id, major_minor, root_dir, mount_point] = fixed;

    auto sep_pos = line.find(" - ");
    if (sep_pos == std::string_view::npos) { return std::move(result); }

    std::array<std::string_view, kSepFields> sep {};
    if (std::ranges::copy(line.substr(sep_pos + 3) | tokenize_sv(' ') | std::views::take(kSepFields), sep.begin()).out
        != sep.end()) {
        return std::move(result);
    }
    auto [fs_type, source] = sep;

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
}

[[nodiscard]] std::string format_device(std::string_view src, std::string_view fs) {
    if (src.empty()) { return {}; }
    if (src == fs) { return std::string(src); }
    return std::format("{} ({})", src, fs);
}

} // namespace

MemInfo SystemInfo::get_memory_status() noexcept {
    const auto [total, initial_available] = []() -> std::pair<std::uint64_t, std::uint64_t> {
        struct sysinfo si {};
        if (posix::expect_result<posix::error_style::posix>(::sysinfo(&si)) && si.mem_unit > 0) {
            const std::uint64_t unit = si.mem_unit;
            return { safe_mul(si.totalram, unit).value_or(0), safe_mul(si.freeram, unit).value_or(0) };
        }
        return { 0, 0 };
    }();

    const std::uint64_t available = [initial_available]() -> std::uint64_t {
        const auto meminfo = read_file("/proc/meminfo");
        if (!meminfo) { return initial_available; }

        constexpr std::array<std::string_view, 1> kKeys = { "MemAvailable" };
        const auto field_opt                            = lookup_info_field(*meminfo, kKeys);
        if (!field_opt) { return initial_available; }

        auto val_str                       = *field_opt;
        constexpr std::string_view kSuffix = "kB";
        if (val_str.ends_with(kSuffix)) { val_str = trim_sv(val_str.substr(0, val_str.size() - kSuffix.size())); }

        const auto res = parse_number<std::uint64_t>(val_str);
        if (!res) { return initial_available; }

        return safe_mul(*res, 1024ULL).value_or(0);
    }();

    const std::uint64_t used = safe_sub(total, available).value_or(0);

    return MemInfo { .total = total, .used = used, .available = available };
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

    const auto used_blocks = safe_sub(vfs->f_blocks, vfs->f_bfree).value_or(0uz);
    info.used              = safe_mul(used_blocks, block_size).value_or(0);

    return info;
}

std::vector<SwapEntry> SystemInfo::get_swaps() noexcept {
    const auto raw_swaps = []() -> std::vector<SwapEntry> {
        const auto content = read_file("/proc/swaps");
        if (!content) { return {}; }
        return *content | split_to_sv('\n') | std::views::drop(1) | std::views::transform(parse_swap_line)
            | std::views::filter([](const auto& swap_result) { return swap_result.has_value(); })
            | std::views::transform([](auto&& swap_result) { return std::move(*swap_result); })
            | std::ranges::to<std::vector>();
    }();

    const auto zswap = []() -> std::optional<SwapEntry> {
        if (!probe::get_zswap_enabled_probe()) { return std::nullopt; }

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

    if (!zswap) { return raw_swaps; }

    auto result = raw_swaps;
    result.push_back(std::move(*zswap));
    return result;
}

std::string SystemInfo::get_device_name(const std::string& path) noexcept {
    auto file_stat = posix::stat(path);
    if (!file_stat) { return "unknown device"; }

    const std::string target_dev = std::format("{}:{}", major(file_stat->st_dev), minor(file_stat->st_dev));

    return parse_file_or(
        "/proc/self/mountinfo",
        [&target_dev, &path](std::string_view content) {
            auto match = std::ranges::fold_left(content | split_to_sv('\n'), MountMatch {},
                [&target_dev, &path](MountMatch&& acc, std::string_view line) {
                    return accumulate_mount_entry(std::move(acc), line, target_dev, path);
                });

            if (!match.exact_src.empty()) { return format_device(match.exact_src, match.exact_fs); }
            if (match.best_len > 0) { return format_device(match.best_src, match.best_fs); }
            return std::string("unknown device");
        },
        std::string { "unknown device" });
}

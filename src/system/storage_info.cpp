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
#include <array>
#include <charconv>
#include <climits>
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

/**
 * @brief Decodes 3-digit octal escape sequences in procfs mount paths.
 *
 * Linux procfs (/proc/self/mountinfo) encodes spaces and special characters as
 * octal escape sequences (e.g., \040 for space) to preserve single-line field delimiters.
 * Unescaping is required before comparing mount points against filesystem paths.
 */
[[nodiscard]] std::string unescape_mount_path(std::string_view sv) {
    if (!sv.contains('\\')) { return std::string { sv }; }

    using namespace std::string_view_literals;

    static constexpr auto kByteChars = []() consteval {
        std::array<char, 256> arr {};
        std::ranges::copy(std::views::iota(std::uint8_t { 0 }) | std::views::take(256), arr.begin());
        return arr;
    }();

    auto chunks { sv | std::views::split('\\') };
    const std::string_view head { *chunks.begin() };

    const auto decode_tail = [](auto&& chunk_range) -> std::array<std::string_view, 2> {
        const std::string_view chunk { chunk_range };
        constexpr std::size_t kOctalDigits { 3 };
        constexpr std::uint8_t kOctalBase { 8 };

        const std::string_view octal_prefix { chunk.substr(0, kOctalDigits) };

        std::uint8_t ch { 0 };
        const auto res { std::from_chars(octal_prefix.data(), octal_prefix.end(), ch, kOctalBase) };
        const bool valid_octal { (octal_prefix.size() == kOctalDigits) && (res.ec == std::errc {})
            && (res.ptr == octal_prefix.end()) };

        if (!valid_octal) { return std::array { "\\"sv, chunk }; }

        const std::string_view decoded_char { &kByteChars[ch], 1 };
        return std::array { decoded_char, chunk.substr(kOctalDigits) };
    };

    auto tail_view { chunks | std::views::drop(1) | std::views::transform(decode_tail) | std::views::join
        | std::views::join };

    const std::string tail_str { std::from_range, tail_view };

    return std::format("{}{}", head, tail_str);
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
    auto [mount_id, parent_id, major_minor, root_dir, raw_mount_point] = fixed;

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

    const std::string mount_point_buf { unescape_mount_path(raw_mount_point) };
    const std::string_view mount_point { mount_point_buf };

    if (!path.starts_with(mount_point)) { return std::move(result); }

    const bool valid_boundary
        = (path.size() == mount_point.size()) || (mount_point == "/") || (path[mount_point.size()] == '/');

    if (!valid_boundary || mount_point.size() <= result.best_len) { return std::move(result); }

    result.best_len = mount_point.size();
    result.best_src = source;
    result.best_fs  = fs_type;

    return std::move(result);
}

[[nodiscard]] MountMatch find_mount_match(
    std::string_view content, std::string_view target_dev, std::string_view path) noexcept {
    return std::ranges::fold_left(
        content | split_to_sv('\n'), MountMatch {}, [&target_dev, &path](MountMatch&& acc, std::string_view line) {
            return accumulate_mount_entry(std::move(acc), line, target_dev, path);
        });
}

[[nodiscard]] std::string format_dev_t(dev_t dev_id) {
    return std::format("{}:{}", major(dev_id), minor(dev_id));
}

[[nodiscard]] std::string get_mount_source(std::string_view path, std::string_view target_dev) {
    const auto content = read_file("/proc/self/mountinfo");
    if (!content) { return {}; }

    const auto match = find_mount_match(*content, target_dev, path);
    if (!match.exact_src.empty()) { return unescape_mount_path(match.exact_src); }
    if (match.best_len > 0) { return unescape_mount_path(match.best_src); }

    return {};
}

[[nodiscard]] std::optional<std::filesystem::path> get_next_slave_path(const std::filesystem::path& sys_path) noexcept {
    std::error_code ec {};
    const std::filesystem::path slaves_dir { sys_path / "slaves" };
    if (!std::filesystem::is_directory(slaves_dir, ec)) { return std::nullopt; }

    const auto dir_iter { std::filesystem::directory_iterator(slaves_dir, ec) };
    if (ec) { return std::nullopt; }

    const auto it { std::ranges::find_if(dir_iter, [](const std::filesystem::directory_entry& entry) noexcept {
        std::error_code canonical_ec {};
        const auto canonical_path { std::filesystem::canonical(entry.path(), canonical_ec) };
        return !canonical_ec && !canonical_path.empty();
    }) };

    if (it == std::default_sentinel) { return std::nullopt; }

    std::error_code canonical_ec {};
    return std::filesystem::canonical(it->path(), canonical_ec);
}

[[nodiscard]] std::filesystem::path resolve_slave_path(std::filesystem::path sys_path) noexcept {
    constexpr std::size_t kMaxSlaveTraversalDepth { posix::kMaxSymloop };
    std::ranges::find_if(std::views::iota(0uz, kMaxSlaveTraversalDepth), [&sys_path](auto) noexcept {
        auto next { get_next_slave_path(sys_path) };
        const bool stop { !next || *next == sys_path };
        if (!stop) { sys_path = std::move(*next); }
        return stop;
    });

    return sys_path;
}

[[nodiscard]] std::optional<std::filesystem::path> resolve_sys_block_path(
    std::string_view path, dev_t dev_id) noexcept {
    using namespace std::string_view_literals;

    const std::string target_dev_str { format_dev_t(dev_id) };

    std::error_code dev_ec {};
    if (auto sys_path { std::filesystem::canonical(std::format("/sys/dev/block/{}", target_dev_str), dev_ec) };
        !dev_ec && !sys_path.empty()) {
        return sys_path;
    }

    const std::string mount_src { get_mount_source(path, target_dev_str) };

    constexpr auto kDevPrefix { "/dev/"sv };
    if (!mount_src.starts_with(kDevPrefix)) { return std::nullopt; }

    const std::string_view dev_name { std::string_view { mount_src }.substr(kDevPrefix.length()) };

    std::error_code class_ec {};
    if (auto sys_class_path { std::filesystem::canonical(std::format("/sys/class/block/{}", dev_name), class_ec) };
        !class_ec && !sys_class_path.empty()) {
        return sys_class_path;
    }

    return std::nullopt;
}

[[nodiscard]] std::filesystem::path resolve_parent_disk_dir(std::filesystem::path sys_path) noexcept {
    std::error_code ec {};
    const auto target_path { resolve_slave_path(std::move(sys_path)) };
    const bool is_partition { std::filesystem::exists(target_path / "partition", ec) };
    return is_partition ? target_path.parent_path() : target_path;
}

[[nodiscard]] ParentDisk extract_parent_disk_info(const std::filesystem::path& parent_dir) noexcept {
    const std::string parent_name { parent_dir.filename().string() };
    if (parent_name.empty()) { return {}; }

    constexpr std::uint16_t kLinuxKernelSectorSize { 512 };
    const auto sectors { read_sysfs_u64(parent_dir / "size").value_or(0) };
    const std::uint64_t total_bytes { safe_mul(sectors, kLinuxKernelSectorSize).value_or(0) };

    std::error_code ec {};
    const std::string dev_path { std::format("/dev/{}", parent_name) };
    const std::string final_path { std::filesystem::exists(dev_path, ec) ? dev_path : parent_name };

    return ParentDisk { .device_path = final_path, .total_bytes = total_bytes };
}

[[nodiscard]] std::string format_device(std::string_view src, std::string_view fs) {
    if (src.empty()) { return {}; }
    const std::string unescaped_src { unescape_mount_path(src) };
    if (unescaped_src == fs) { return unescaped_src; }
    return std::format("{} ({})", unescaped_src, fs);
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

DiskInfo SystemInfo::get_disk_usage(std::string_view mountpoint) noexcept {
    DiskInfo info {};
    const auto vfs = posix::statvfs(mountpoint);
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

std::string SystemInfo::get_device_name(std::string_view path) noexcept {
    const auto file_stat = posix::stat(path);
    if (!file_stat) { return "unknown device"; }

    const dev_t dev_id { S_ISBLK(file_stat->st_mode) ? file_stat->st_rdev : file_stat->st_dev };
    const std::string target_dev { format_dev_t(dev_id) };
    const auto content = read_file("/proc/self/mountinfo");
    if (!content) { return "unknown device"; }

    const auto match = find_mount_match(*content, target_dev, path);
    if (!match.exact_src.empty()) { return format_device(match.exact_src, match.exact_fs); }
    if (match.best_len > 0) { return format_device(match.best_src, match.best_fs); }

    return "unknown device";
}

ParentDisk SystemInfo::get_parent_disk(std::string_view path) noexcept {
    return posix::stat(path)
        .transform([](const struct ::stat& st) noexcept { return S_ISBLK(st.st_mode) ? st.st_rdev : st.st_dev; })
        .transform([](dev_t dev_id) noexcept { return std::optional { dev_id }; })
        .value_or(std::nullopt)
        .and_then([path](dev_t dev_id) noexcept { return resolve_sys_block_path(path, dev_id); })
        .transform([](std::filesystem::path sys_path) noexcept { return resolve_parent_disk_dir(std::move(sys_path)); })
        .transform(extract_parent_disk_info)
        .value_or(ParentDisk {});
}

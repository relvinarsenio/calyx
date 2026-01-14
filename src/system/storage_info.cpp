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
#include <cctype>
#include <charconv>
#include <expected>
#include <fstream>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>

namespace {

// C++23: Helper to capitalize using ranges/views if complex, but simple version is fine.
// Improved to be safe and use string views where possible, returning a string.
std::string capitalize(std::string_view text) {
    if (text.empty())
        return {};

    std::string ret(text);
    if (!ret.empty()) {
        ret[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ret[0])));
    }

    if (ret == "Zram")
        return "ZRAM";
    return ret;
}

// C++23: Robust number parsing helper
template <typename T>
std::expected<T, std::errc> parse_number(std::string_view sv) {
    T value;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec == std::errc()) {
        return value;
    }
    return std::unexpected(ec);
}

}  // namespace

MemInfo SystemInfo::get_memory_status() {
    MemInfo info{};
    struct sysinfo si;

    bool sysinfo_ok = (sysinfo(&si) == 0);
    if (sysinfo_ok) {
        info.total = static_cast<uint64_t>(si.totalram) * si.mem_unit;
        // Fallback available memory from sysinfo (usually freeram + buffers + cached)
        // But MemAvailable from /proc/meminfo is more accurate for "allocatable" memory.
        info.available = static_cast<uint64_t>(si.freeram) * si.mem_unit;
    }

    // Modern C++23 file reading with views
    std::ifstream meminfo("/proc/meminfo");
    std::string line;

    while (std::getline(meminfo, line)) {
        std::string_view sv = line;

        // Check for MemAvailable
        if (sv.starts_with("MemAvailable:")) {
            auto colon = sv.find(':');
            if (colon != std::string_view::npos) {
                // Drop prefix and colon
                sv = sv.substr(colon + 1);
                // Trim leading whitespace
                sv = trim_sv(sv);

                // Parse the number (usually in kB)
                // MemAvailable:    8056220 kB
                auto parts = sv | std::views::split(' ') | std::views::take(1);
                if (!parts.empty()) {
                    auto rng = *parts.begin();
                    std::string_view num_sv(rng.begin(), rng.end());

                    if (auto val = parse_number<uint64_t>(num_sv)) {
                        info.available = *val * 1024;  // Check if unit is kB, usually is.
                    }
                }
                break;  // Found it
            }
        }
    }

    if (info.total >= info.available) {
        info.used = info.total - info.available;
    } else {
        info.used = 0;
    }

    return info;
}

DiskInfo SystemInfo::get_disk_usage(const std::string& mountpoint) {
    DiskInfo info{};
    struct statvfs disk;

    if (statvfs(mountpoint.c_str(), &disk) == 0) {
        info.total = static_cast<uint64_t>(disk.f_blocks) * disk.f_frsize;
        info.free = static_cast<uint64_t>(disk.f_bfree) * disk.f_frsize;
        info.available = static_cast<uint64_t>(disk.f_bavail) * disk.f_frsize;

        auto used_blocks = (disk.f_blocks > disk.f_bfree ? disk.f_blocks - disk.f_bfree : 0);
        info.used = static_cast<uint64_t>(used_blocks) * disk.f_frsize;
    }

    return info;
}

std::vector<SwapEntry> SystemInfo::get_swaps() {
    std::vector<SwapEntry> swaps;

    // Use C++ streams but process lines with ranges
    std::ifstream swaps_file("/proc/swaps");
    std::string line;

    // Skip header line
    if (std::getline(swaps_file, line)) {
        while (std::getline(swaps_file, line)) {
            // /proc/swaps format:
            // Filename Type Size Used Priority
            // Tokenize by whitespace

            // C++23 range-based tokenization
            auto tokens = line | std::views::split(' ') |
                          std::views::filter([](auto&& rng) { return !std::ranges::empty(rng); }) |
                          std::views::transform(
                              [](auto&& rng) { return std::string_view(rng.begin(), rng.end()); });

            auto it = tokens.begin();
            auto end = tokens.end();

            if (it == end)
                continue;
            std::string_view path(*it++);
            if (it == end)
                continue;
            std::string_view type(*it++);
            if (it == end)
                continue;
            std::string_view size_str(*it++);
            if (it == end)
                continue;
            std::string_view used_str(*it++);
            // We ignore priority usually

            SwapEntry entry;
            entry.path = std::string(path);

            if (path.find("zram") != std::string_view::npos) {
                entry.type = "ZRAM";
            } else {
                entry.type = capitalize(type);
            }

            if (auto val = parse_number<uint64_t>(size_str))
                entry.size = *val * 1024;
            if (auto val = parse_number<uint64_t>(used_str))
                entry.used = *val * 1024;

            swaps.push_back(std::move(entry));
        }
    }

    // Check ZSwap
    std::ifstream zswap_file("/sys/module/zswap/parameters/enabled");
    char c;
    if (zswap_file >> c && (c == 'Y' || c == 'y' || c == '1')) {
        SwapEntry zswap;
        zswap.type = "ZSwap";
        zswap.path = "Enabled";
        zswap.size = 0;
        zswap.used = 0;
        zswap.is_zswap = true;
        swaps.push_back(zswap);
    }

    return swaps;
}

std::string SystemInfo::get_device_name(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
        return "unknown device";

    std::ifstream mountinfo("/proc/self/mountinfo");
    if (!mountinfo)
        return "unknown device";

    const std::string target_dev = std::format("{}:{}", major(st.st_dev), minor(st.st_dev));

    std::string best_path_match = "unknown device";
    size_t best_path_len = 0;

    std::string exact_dev_match;

    std::string line;
    while (std::getline(mountinfo, line)) {
        // C++23: Clean tokenization
        auto tokens_view = line | std::views::split(' ') |
                           std::views::filter([](auto&& rng) { return !std::ranges::empty(rng); }) |
                           std::views::transform([](auto&& rng) {
                               return std::string_view(
                                   &*rng.begin(), static_cast<size_t>(std::ranges::distance(rng)));
                           });

        // Convert to vector for indexed access (parsing the mountinfo structure)
        // mountinfo structure is slightly variable but fields 0-4 are fixed
        // 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
        // (0)ID (1)Parent (2)Maj:Min (3)Root (4)MountPoint ... - (N)FSType (N+1)Source

        std::vector<std::string_view> tokens;
        for (auto t : tokens_view)
            tokens.push_back(t);

        if (tokens.size() < 7)  // Min required to even reach the separator
            continue;

        std::string_view major_minor = tokens[2];
        std::string_view mount_point = tokens[4];

        // Find separator "-"
        // It's optional fields between MountPoint and separator
        auto separator_it = std::ranges::find(tokens, "-");
        if (separator_it == tokens.end() || std::distance(separator_it, tokens.end()) < 3)
            continue;

        // FSType is after separator, Source is after FSType
        std::string_view fs_type = *(separator_it + 1);
        std::string_view source = *(separator_it + 2);

        auto format_output = [&](std::string_view src, std::string_view fs) {
            if (src == fs)
                return std::string(src);
            return std::format("{} ({})", src, fs);
        };

        if (major_minor == target_dev) {
            exact_dev_match = format_output(source, fs_type);
        }

        if (path.starts_with(mount_point)) {  // C++20 starts_with
            bool valid_boundary = (path.size() == mount_point.size()) || (mount_point == "/") ||
                                  (path[mount_point.size()] == '/');

            if (valid_boundary && mount_point.size() > best_path_len) {
                best_path_len = mount_point.size();
                best_path_match = format_output(source, fs_type);
            }
        }
    }

    if (best_path_len > 0 && best_path_match != "unknown device") {
        return best_path_match;
    }

    if (!exact_dev_match.empty()) {
        return exact_dev_match;
    }

    return best_path_match;
}

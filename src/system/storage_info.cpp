/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/system_info.hpp"
#include "include/utils.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>

namespace {

std::string capitalize(std::string_view text) {
    if (text.empty())
        return {};
    std::string ret(text);
    ret[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ret[0])));
    if (ret == "Zram")
        return "ZRAM";
    return ret;
}

}  // namespace

MemInfo SystemInfo::get_memory_status() {
    MemInfo info{};
    struct sysinfo si;

    bool sysinfo_ok = (sysinfo(&si) == 0);
    if (sysinfo_ok) {
        info.total = static_cast<uint64_t>(si.totalram) * si.mem_unit;
    }

    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    uint64_t mem_available = 0;

    while (std::getline(meminfo, line)) {
        if (line.starts_with("MemAvailable:")) {
            std::string_view sv = line;
            auto colon = sv.find(':');
            if (colon != std::string_view::npos) {
                sv = sv.substr(colon + 1);
                sv = trim_sv(sv);

                auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), mem_available);
                if (ec == std::errc()) {
                    mem_available *= 1024;
                } else {
                    mem_available = 0;
                }
                break;
            }
        }
    }

    if (mem_available > 0) {
        info.available = mem_available;
    } else if (sysinfo_ok) {
        info.available = static_cast<uint64_t>(si.freeram) * si.mem_unit;
    } else {
        info.available = 0;
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

    std::ifstream swaps_file("/proc/swaps");
    std::string line;

    if (std::getline(swaps_file, line)) {
        while (std::getline(swaps_file, line)) {
            std::stringstream ss(line);
            std::string path, type, size_str, used_str;

            if (ss >> path >> type >> size_str >> used_str) {
                SwapEntry entry;
                entry.path = path;

                if (path.find("zram") != std::string::npos) {
                    entry.type = "ZRAM";
                } else {
                    entry.type = capitalize(type);
                }

                uint64_t val_size = 0, val_used = 0;
                auto [p1, ec1] =
                    std::from_chars(size_str.data(), size_str.data() + size_str.size(), val_size);
                auto [p2, ec2] =
                    std::from_chars(used_str.data(), used_str.data() + used_str.size(), val_used);

                if (ec1 == std::errc() && ec2 == std::errc()) {
                    entry.size = val_size * 1024;
                    entry.used = val_used * 1024;
                    swaps.push_back(entry);
                }
            }
        }
    }

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
        std::string_view line_view(line);
        std::vector<std::string_view> tokens;

        size_t start = 0;
        while (start < line_view.length()) {
            while (start < line_view.length() &&
                   (line_view[start] == ' ' || line_view[start] == '\t'))
                start++;
            if (start >= line_view.length())
                break;

            size_t end = start;
            while (end < line_view.length() && line_view[end] != ' ' && line_view[end] != '\t')
                end++;

            tokens.push_back(line_view.substr(start, end - start));
            start = end;
        }

        if (tokens.size() < 5)
            continue;

        std::string id(tokens[0]), parent(tokens[1]), major_minor(tokens[2]), root(tokens[3]),
            mount_point(tokens[4]);

        size_t dash_pos = 5;
        while (dash_pos < tokens.size() && tokens[dash_pos] != "-")
            dash_pos++;

        if (dash_pos + 2 >= tokens.size())
            continue;

        std::string fs_type(tokens[dash_pos + 1]), source(tokens[dash_pos + 2]);

        auto format_output = [&](const std::string& src, const std::string& fs) {
            if (src == fs)
                return src;
            return std::format("{} ({})", src, fs);
        };

        if (major_minor == target_dev) {
            exact_dev_match = format_output(source, fs_type);
        }

        if (path.compare(0, mount_point.size(), mount_point) == 0) {
            bool valid_boundary = path.size() == mount_point.size() || mount_point == "/" ||
                                  path[mount_point.size()] == '/';

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
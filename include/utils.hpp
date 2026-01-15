/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <print>
#include <sys/ioctl.h>
#include <unistd.h>

#include "config.hpp"

namespace fs = std::filesystem;

inline std::size_t get_term_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return std::min(static_cast<std::size_t>(w.ws_col), Config::TERM_WIDTH);
    }
    return Config::TERM_WIDTH;
}

inline void print_line() {
    std::size_t width = get_term_width();
    std::println("{:-<{}}", "", width);
}

inline void print_centered_header(std::string_view text) {
    std::size_t width = get_term_width();
    std::size_t text_len = text.length();

    if (text_len >= width - 2) {
        std::println("{}", text);
        return;
    }

    std::size_t remaining = width - text_len - 2;
    std::size_t left_pad = remaining / 2;
    std::size_t right_pad = remaining - left_pad;

    std::println("{0:-<{1}} {2} {0:-<{3}}", "", left_pad, text, right_pad);
}

[[nodiscard]] constexpr std::string_view trim_sv(std::string_view str) noexcept {
    auto first = str.find_first_not_of(" \t\n\r\v\f");
    if (first == std::string_view::npos)
        return {};
    auto last = str.find_last_not_of(" \t\n\r\v\f");
    return str.substr(first, last - first + 1);
}

[[nodiscard]] inline std::string trim(const std::string& str) {
    return std::string(trim_sv(str));
}

inline std::string format_bytes(std::uint64_t bytes) {
    if (bytes == 0)
        return "0 B";

    static constexpr std::array units = {"B", "KB", "MB", "GB", "TB"};

    std::size_t i = 0;
    double d = static_cast<double>(bytes);
    while (d >= 1024 && i < units.size() - 1) {
        d /= 1024;
        i++;
    }
    return std::format("{:.1f} {}", d, units[i]);
}

inline std::filesystem::path get_exe_dir() {
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && exe.has_parent_path())
        return exe.parent_path();
    return std::filesystem::current_path();
}

inline bool is_disk_space_available(const std::filesystem::path& path,
                                    std::uint64_t required_bytes) {
    std::error_code ec;
    auto space_info = fs::space(path, ec);
    if (ec) {
        return false;
    }

    if (space_info.available < Config::MIN_BUFFER_BYTES) {
        return false;
    }

    return (space_info.available - Config::MIN_BUFFER_BYTES) >= required_bytes;
}

inline void cleanup_artifacts() {
    const auto exe_dir = get_exe_dir();

    for (std::string_view filename :
         {Config::SPEEDTEST_TGZ, std::string_view("speedtest-cli"), Config::TEST_FILENAME}) {
        std::error_code ec;

        if (fs::exists(filename, ec)) {
            fs::remove_all(filename, ec);
        }

        auto abs_path = exe_dir / filename;
        if (fs::exists(abs_path, ec)) {
            fs::remove_all(abs_path, ec);
        }
    }
}
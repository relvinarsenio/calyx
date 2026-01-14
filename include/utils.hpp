/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

void print_line();
void print_centered_header(std::string_view text);

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

std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] bool is_disk_space_available(const std::filesystem::path& path,
                                           std::uint64_t required_bytes);
void cleanup_artifacts();
std::filesystem::path get_exe_dir();
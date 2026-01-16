/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace Color {
constexpr std::string_view RESET = "\033[0m";
constexpr std::string_view RED = "\033[31m";
constexpr std::string_view GREEN = "\033[32m";
constexpr std::string_view YELLOW = "\033[33m";
constexpr std::string_view CYAN = "\033[36m";
constexpr std::string_view BOLD = "\033[1m";

inline std::string colorize(std::string_view text, std::string_view color) {
    return std::format("{}{}{}", color, text, RESET);
}
}  // namespace Color

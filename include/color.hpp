/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace color {
inline constexpr std::string_view kReset  = "\033[0m";
inline constexpr std::string_view kRed    = "\033[31m";
inline constexpr std::string_view kGreen  = "\033[32m";
inline constexpr std::string_view kYellow = "\033[33m";
inline constexpr std::string_view kCyan   = "\033[36m";
inline constexpr std::string_view kBold   = "\033[1m";

[[nodiscard]] inline std::string colorize(std::string_view text, std::string_view color_code) {
    return std::format("{}{}{}", color_code, text, kReset);
}
} // namespace color

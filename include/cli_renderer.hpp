/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "results.hpp"
#include "speed_test.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

/**
 * @namespace ui
 * @brief Provides functions for rendering speed test results and progress bars to the terminal.
 */
namespace ui {
/**
 * @brief Renders the final results of a speed test in a formatted table.
 * @param result The SpeedTestResult object containing measured speeds and latencies.
 */
void render_speed_results(const SpeedTestResult& result);

/**
 * @brief Creates a callback function that renders a spinner for indeterminate progress.
 * @return A SpinnerCallback that can be invoked during long-running operations.
 */
SpinnerCallback make_spinner_callback();

/**
 * @brief Creates a callback function for rendering a deterministic progress bar.
 * @param label_width The width of the text label displayed alongside the bar.
 * @return A move-only function that updates progress based on (current, total, label).
 */
std::move_only_function<void(std::size_t, std::size_t, std::string_view) const noexcept> make_progress_callback(
    std::uint8_t label_width);

/**
 * @brief Renders a single line representing progress.
 * @param label       The text label to display (e.g., "Downloading").
 * @param percent     The current progress percentage (0-100).
 * @param label_width The fixed width allocated for the label.
 */
void render_progress_line(std::string_view label, std::size_t percent, std::uint8_t label_width);

/**
 * @brief Formats the ZSwap compression ratio (e.g., "3.99x").
 * @param uncompressed_bytes Logical size in bytes.
 * @param compressed_bytes Actual RAM consumed in bytes.
 * @return Formatted ratio string.
 */
[[nodiscard]] std::string format_zswap_ratio(std::uint64_t uncompressed_bytes, std::uint64_t compressed_bytes);
} // namespace ui

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
#include <string_view>

/**
 * @namespace ui
 * @brief Provides functions for rendering speed test results and progress bars to the terminal.
 */
namespace ui {

/**
 * @brief Escape sequences for text styling (Color, Bold, etc.)
 */
namespace style {
inline constexpr std::string_view reset    = "\x1b[0m";
inline constexpr std::string_view bold     = "\x1b[1m";
inline constexpr std::string_view italic   = "\x1b[3m";
inline constexpr std::string_view unitalic = "\x1b[23m";
inline constexpr std::string_view dim      = "\x1b[2m";
/**
 * @brief Resets text intensity to normal (neither bold nor dim).
 * @note ESC[22m turns off both bold (extra intensity) and dim (faint/reduced intensity) per ANSI standard.
 */
inline constexpr std::string_view normal_intensity = "\x1b[22m";
inline constexpr std::string_view underline        = "\x1b[4m";
inline constexpr std::string_view clear_line       = "\x1b[2K";
} // namespace style

/**
 * @brief Escape sequences for cursor movement.
 */
namespace cursor {
inline constexpr std::string_view hide = "\x1b[?25l";
inline constexpr std::string_view show = "\x1b[?25h";
} // namespace cursor

/**
 * @brief Escape sequences for terminal state management.
 */
namespace term {
inline constexpr std::string_view sync_start   = "\x1b[?2026h";
inline constexpr std::string_view sync_end     = "\x1b[?2026l";
inline constexpr std::string_view clear_screen = "\x1b[H\x1b[2J\x1b[3J";
} // namespace term

/**
 * @brief RAII Guard to ensure terminal is in a clean state during and after UI execution.
 * @details Hides cursor on construction, shows cursor on destruction.
 */
class TerminalGuard {
    bool active_ = false;

public:
    TerminalGuard();
    ~TerminalGuard();

    TerminalGuard(const TerminalGuard&)            = delete;
    TerminalGuard& operator=(const TerminalGuard&) = delete;
    TerminalGuard(TerminalGuard&&)                 = delete;
    TerminalGuard& operator=(TerminalGuard&&)      = delete;
};

/**
 * @brief Renders the final results of a speed test in a formatted table.
 * @param result The SpeedTestResult object containing measured speeds and latencies.
 */
void render_speed_results(const SpeedTestResult& result);

/**
 * @brief Renders the result of a single disk benchmark run.
 * @param result The result to render.
 */
void print_disk_run_result(const DiskIORunResult& result);

/**
 * @brief Renders the final average summary and latency tables of a disk benchmark.
 * @param disk_runs The list of disk benchmark results for each run.
 * @param cycles_to_ns The hardware calibration multiplier to convert cycles to nanoseconds.
 */
void render_disk_results_summary(std::span<const DiskIORunResult> disk_runs, double cycles_to_ns);

/**
 * @brief RAII Spinner that displays a progress animation in a background thread.
 * @details Starts spinning on construction, stops and cleans up terminal on destruction.
 */
class ScopedSpinner {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit ScopedSpinner(std::string_view label);
    ~ScopedSpinner() noexcept;

    ScopedSpinner(const ScopedSpinner&)            = delete;
    ScopedSpinner& operator=(const ScopedSpinner&) = delete;
    ScopedSpinner(ScopedSpinner&&) noexcept;
    ScopedSpinner& operator=(ScopedSpinner&&) noexcept;
};

/**
 * @brief Creates a callback function for rendering a deterministic progress bar.
 * @param label_width The width of the text label displayed alongside the bar.
 * @return A move-only function that updates progress based on (current, total, label).
 */
std::move_only_function<void(std::size_t, std::size_t, std::string_view) const> make_progress_callback(
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

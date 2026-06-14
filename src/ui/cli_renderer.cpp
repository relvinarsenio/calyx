/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "cli_renderer.hpp"

#include "color.hpp"
#include "config.hpp"
#include "file_descriptor.hpp"
#include "interrupts.hpp"
#include "locale_probe.hpp"
#include "scope.hpp"
#include "speed_test.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace ui {

namespace {

[[nodiscard]] std::string format_speed(double mbps) {
    if (mbps >= config::kMbpsToGbpsThreshold) {
        return std::format("{:.2f} Gbps", mbps / config::kMbpsToGbpsThreshold);
    }
    return std::format("{:.2f} Mbps", mbps);
}

/**
 * @brief Builds a filled progress bar string for the given percentage.
 *
 * Uses block characters when the locale supports UTF-8, ASCII fallback otherwise.
 * Returns `std::string` — not `std::string_view` — to give callers stable ownership.
 */
[[nodiscard]] std::string build_progress_bar(std::size_t percent) {
    static thread_local std::string bar_buffer;

    const std::size_t required_capacity = safe_mul(toSize(config::kProgressBarWidth), 3uz).value_or(0uz);
    if (bar_buffer.capacity() < required_capacity) { bar_buffer.reserve(required_capacity); }
    bar_buffer.clear();

    percent = std::clamp(percent, 0uz, 100uz);

    const std::size_t max_width = toSize(config::kProgressBarWidth);
    const std::size_t filled    = std::min(safe_mul(percent, max_width).value_or(0uz) / 100uz, max_width);

    const bool use_ascii              = config::kUiForceAscii || !locale::supports_utf8();
    const std::string_view fill_char  = use_ascii ? "#" : "\u2588";
    const std::string_view empty_char = use_ascii ? "-" : "\u2591";

    auto append = [](std::string_view ch) { bar_buffer += ch; };
    std::ranges::for_each(std::views::repeat(fill_char, filled), append);
    std::ranges::for_each(std::views::repeat(empty_char, max_width - filled), append);

    return bar_buffer;
}

void print_spinner_frame(std::string_view text, std::string_view frame, double elapsed_s) {
    std::print("{}\r {:<{}} {} {:4.1f}s{}", term::sync_start, text, config::kProgressBarWidth, frame, elapsed_s,
        term::sync_end);
    std::fflush(stdout);
}

void clear_spinner_line() noexcept {
    std::print("\r{}", style::clear_line);
    std::fflush(stdout);
}

/** @brief Shared mutable state between the progress UI thread and its update callback. */
struct ProgressState {
    std::atomic<std::size_t> current { 0uz };
    std::atomic<std::size_t> total { 0uz };
    std::string label;
    mutable std::mutex label_mtx;
};

[[nodiscard]] std::string read_label(const ProgressState& state) {
    std::lock_guard lk(state.label_mtx);
    return state.label;
}

void finalize_progress(const ProgressState& state, std::uint8_t label_width) noexcept {
    const std::size_t curr = state.current.load(std::memory_order_acquire);
    const std::size_t tot  = state.total.load(std::memory_order_acquire);
    if (tot == 0uz || curr < tot) {
        clear_spinner_line();
    } else {
        render_progress_line(read_label(state), 100uz, label_width);
    }
}

/**
 * @brief Background thread body for the deterministic progress bar.
 *
 * Separated from `make_progress_callback` so concurrency orchestration
 * and callback factory each have a single reason to change.
 */
void progress_ui_thread_body(std::stop_token st, std::shared_ptr<ProgressState> state, std::uint8_t label_width) {
    const auto flush_final = [&state, label_width] { finalize_progress(*state, label_width); };

    scope_exit final_render { flush_final };

    std::size_t last_current = std::numeric_limits<std::size_t>::max();

    while (!st.stop_requested() && !check_interrupted()) {
        const std::size_t curr = state->current.load(std::memory_order_acquire);
        const std::size_t tot  = state->total.load(std::memory_order_acquire);

        if (curr == last_current || tot == 0uz) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config::kUiUpdateIntervalMs));
            continue;
        }

        const std::size_t percent = safe_mul(curr, 100uz).value_or(0uz) / tot;
        render_progress_line(read_label(*state), percent, label_width);
        last_current = curr;
        std::this_thread::sleep_for(std::chrono::milliseconds(config::kUiUpdateIntervalMs));
    }
}

[[nodiscard]] std::string align_left(std::string_view str, std::size_t width) {
    return std::format("{:<{}}", str, width);
}

void print_header() {
    std::println("{}{}{}{}{}", align_left(" Node Name", config::kUiTableNodeWidth),
        align_left("Download", config::kUiTableDlWidth), align_left("Upload", config::kUiTableUlWidth),
        align_left("Latency", config::kUiTableLatencyWidth), align_left("Loss", config::kUiTableLossWidth));
}

void print_error_row(const SpeedEntryResult& entry) {
    const std::string err = truncate_error(entry.error);
    std::println("{} {}{}Error: {}{}", color::kYellow, align_left(entry.node_name, config::kUiTableNodeWidth - 1uz),
        color::kRed, err, color::kReset);
}

void print_success_row(const SpeedEntryResult& entry) {
    const std::string latency_str = (entry.latency_ms > 0.0) ? std::format("{:.2f} ms", entry.latency_ms) : "-";
    std::println("{} {}{}{}{}{}{}{}{}{}{}", color::kYellow,
        align_left(entry.node_name, config::kUiTableNodeWidth - 1uz), color::kGreen,
        align_left(format_speed(entry.download_mbps), config::kUiTableDlWidth), color::kRed,
        align_left(format_speed(entry.upload_mbps), config::kUiTableUlWidth), color::kCyan,
        align_left(latency_str, config::kUiTableLatencyWidth), color::kRed,
        align_left(entry.loss.empty() ? "-" : entry.loss, config::kUiTableLossWidth), color::kReset);
}

} // namespace

TerminalGuard::TerminalGuard() noexcept {
    if (posix::file_descriptor::is_tty(posix::file_descriptor::stdout_fd)) {
        active_ = true;
        std::print("{}", cursor::hide);
        std::fflush(stdout);
    }
}

TerminalGuard::~TerminalGuard() noexcept {
    if (active_) {
        std::print("{}{}", style::reset, cursor::show);
        std::fflush(stdout);
    }
}

[[nodiscard]] std::string format_zswap_ratio(std::uint64_t uncompressed_bytes, std::uint64_t compressed_bytes) {
    if (uncompressed_bytes == 0uz) { return "Idle"; }
    if (compressed_bytes == 0uz) { return "Max"; }
    const double ratio = toDouble(uncompressed_bytes) / toDouble(compressed_bytes);
    return std::format("{:.2f}×", ratio);
}

void render_speed_results(const SpeedTestResult& result) {
    print_header();

    constexpr auto is_success = [](const auto& entry) { return entry.success; };
    constexpr auto is_error   = [](const auto& entry) { return !entry.success; };

    std::ranges::for_each(result.entries | std::views::filter(is_success), print_success_row);
    std::ranges::for_each(result.entries | std::views::filter(is_error), print_error_row);
}

/**
 * @brief Internal execution context for the CLI progress spinner.
 *
 * Encapsulates background rendering state and synchronization primitives,
 * isolating threading dependencies from the public API header.
 */
struct ScopedSpinner::Impl {
    std::string text_;
    std::chrono::steady_clock::time_point start_;
    mutable std::mutex mtx_;
    std::condition_variable_any stop_cv_;
    std::span<const std::string_view> frames_ {};
    std::jthread worker_;

    /** @brief Atomically reads the current display text and start time under the mutex. */
    [[nodiscard]] std::pair<std::string, std::chrono::steady_clock::time_point> snapshot() const {
        std::lock_guard lk(mtx_);
        return { text_, start_ };
    }

    /** @brief Renders one spinner frame at the given index. */
    void tick(std::size_t idx) {
        const auto [text, t0] = snapshot();
        const double elapsed  = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        print_spinner_frame(text, frames_[idx % frames_.size()], elapsed);
    }

    void worker_loop(std::stop_token st) {
        scope_exit cleanup { clear_spinner_line };

        auto ticks = std::views::iota(0uz)
            | std::views::take_while([&st](auto) { return !st.stop_requested() && !check_interrupted(); });

        std::ranges::for_each(ticks, [this, &st](std::size_t idx) {
            tick(idx);
            std::unique_lock lk(mtx_);
            stop_cv_.wait_for(lk, st, std::chrono::milliseconds(config::kUiSpinnerDelayMs), check_interrupted);
        });
    }

    explicit Impl(std::span<const std::string_view> frames, std::string_view text)
        : text_(text)
        , start_(std::chrono::steady_clock::now())
        , frames_(frames)
        , worker_(std::bind_front(&Impl::worker_loop, this)) {}

    ~Impl() noexcept {
        worker_.request_stop();
        stop_cv_.notify_all();
    }
};

ScopedSpinner::ScopedSpinner(std::string_view label) {
    static constexpr std::array<std::string_view, 10uz> kUtfFrames
        = { "\u280B", "\u2819", "\u2839", "\u2838", "\u283C", "\u2834", "\u2826", "\u2827", "\u2807", "\u280F" };
    static constexpr std::array<std::string_view, 4uz> kAsciiFrames = { "|", "/", "-", "\\" };

    auto frames = (config::kUiForceAscii || !locale::supports_utf8()) ? std::span<const std::string_view>(kAsciiFrames)
                                                                      : std::span<const std::string_view>(kUtfFrames);
    impl_       = std::make_unique<Impl>(frames, label);
}

ScopedSpinner::~ScopedSpinner() noexcept                          = default;
ScopedSpinner::ScopedSpinner(ScopedSpinner&&) noexcept            = default;
ScopedSpinner& ScopedSpinner::operator=(ScopedSpinner&&) noexcept = default;

std::move_only_function<void(std::size_t, std::size_t, std::string_view) const> make_progress_callback(
    std::uint8_t label_width) {

    auto state = std::make_shared<ProgressState>();
    std::jthread ui_thread(
        [state, label_width](std::stop_token st) { progress_ui_thread_body(std::move(st), state, label_width); });

    return [state = std::move(state), jth = std::make_shared<std::jthread>(std::move(ui_thread))](
               std::size_t current, std::size_t total, std::string_view label) {
        {
            std::lock_guard lk(state->label_mtx);
            state->label = label;
        }
        state->total.store(total, std::memory_order_release);
        state->current.store(current, std::memory_order_release);
    };
}

void render_progress_line(std::string_view label, std::size_t percent, std::uint8_t label_width) {
    percent               = std::clamp(percent, 0uz, 100uz);
    const std::string bar = build_progress_bar(percent);

    std::print("{}\r{} {:<{}} [{}] {:3}%{}", term::sync_start, style::clear_line, label, label_width, bar, percent,
        term::sync_end);
    std::fflush(stdout);
}

} // namespace ui

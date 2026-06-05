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
#include "speed_test.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ui {

namespace {

const bool kSupportsUtf8 = []() {
    auto is_utf8 = [](std::string_view env_str) -> bool {
        constexpr std::array<std::string_view, 2> kKeywords = { "utf-8", "utf8" };
        return std::ranges::any_of(kKeywords, [env_str](std::string_view kw) {
            return std::ranges::contains_subrange(
                env_str, kw, [](char lhs, char rhs) { return std::tolower(toUChar(lhs)) == rhs; });
        });
    };

    auto check = [&is_utf8](const char* ptr) -> bool { return ptr && *ptr && is_utf8(std::string_view(ptr)); };

    return check(std::getenv("LC_ALL")) || check(std::getenv("LC_CTYPE")) || check(std::getenv("LANG"));
}();

class UiSpinner {
    std::string text_;
    std::chrono::steady_clock::time_point start_;
    std::span<const std::string_view> frames_ {};
    std::atomic<bool> spinning_ { false };
    std::atomic<std::uint32_t> epoch_ { 0 };
    std::atomic<std::uint32_t> epoch_done_ { 0 };
    std::jthread worker_;

    void render_spin_cycle(std::stop_token& st) {
        std::size_t idx = 0;
        std::mutex mtx;
        std::unique_lock lock(mtx);
        std::condition_variable_any cv;

        while (spinning_.load(std::memory_order_acquire) && !st.stop_requested()) {
            double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
            auto frame     = frames_[idx++ % frames_.size()];

            std::print("\r {:<{}} {} {:4.1f}s", text_, config::kProgressBarWidth, frame, elapsed);
            std::fflush(stdout);
            cv.wait_for(lock, st, std::chrono::milliseconds(config::kUiSpinnerDelayMs), [] { return false; });
        }

        std::print("\r\x1b[2K");
        std::fflush(stdout);
    }

    void worker_loop(std::stop_token st) {
        std::uint32_t seen = 0;

        while (!st.stop_requested()) {
            if (epoch_.load(std::memory_order_acquire) == seen) {
                epoch_.wait(seen, std::memory_order_acquire);
                continue;
            }
            seen = epoch_.load(std::memory_order_relaxed);

            render_spin_cycle(st);
            epoch_done_.store(seen, std::memory_order_release);
            epoch_done_.notify_one();
        }
    }

public:
    UiSpinner(const UiSpinner&)            = delete;
    UiSpinner& operator=(const UiSpinner&) = delete;
    UiSpinner(UiSpinner&&)                 = delete;
    UiSpinner& operator=(UiSpinner&&)      = delete;
    UiSpinner() {
        static constexpr std::array<std::string_view, 10> kUtfFrames
            = { "\u280B", "\u2819", "\u2839", "\u2838", "\u283C", "\u2834", "\u2826", "\u2827", "\u2807", "\u280F" };
        static constexpr std::array<std::string_view, 4> kAsciiFrames = { "|", "/", "-", "\\" };

        frames_ = (config::kUiForceAscii || !kSupportsUtf8) ? std::span<const std::string_view>(kAsciiFrames)
                                                            : std::span<const std::string_view>(kUtfFrames);

        worker_ = std::jthread([this](std::stop_token st) { worker_loop(std::move(st)); });
    }

    ~UiSpinner() {
        worker_.request_stop();
        epoch_.fetch_add(1, std::memory_order_release);
        epoch_.notify_one();
    }

    void start(std::string_view text) {
        if (spinning_.load(std::memory_order_relaxed)) { stop(); }
        text_  = text;
        start_ = std::chrono::steady_clock::now();
        spinning_.store(true, std::memory_order_relaxed);
        epoch_.fetch_add(1, std::memory_order_release);
        epoch_.notify_one();
    }

    void stop() {
        auto target = epoch_.load(std::memory_order_relaxed);
        spinning_.store(false, std::memory_order_release);
        std::uint32_t done = epoch_done_.load(std::memory_order_acquire);
        while (done < target) {
            epoch_done_.wait(done, std::memory_order_acquire);
            done = epoch_done_.load(std::memory_order_acquire);
        }
    }
};

[[nodiscard]] std::string format_speed(double mbps) {
    if (mbps >= config::kMbpsToGbpsThreshold) {
        return std::format("{:.2f} Gbps", mbps / config::kMbpsToGbpsThreshold);
    }
    return std::format("{:.2f} Mbps", mbps);
}

std::string_view create_progress_bar_sv(std::size_t percent) {
    static thread_local std::string bar_buffer;

    if (bar_buffer.capacity() < config::kProgressBarWidth * 3) { bar_buffer.reserve(config::kProgressBarWidth * 3); }
    bar_buffer.clear();

    percent = std::clamp(percent, 0uz, 100uz);

    const std::size_t max_width = std::size_t { config::kProgressBarWidth };
    const std::size_t filled    = std::min((percent * max_width) / 100uz, max_width);

    const bool use_ascii              = config::kUiForceAscii || !kSupportsUtf8;
    const std::string_view fill_char  = use_ascii ? "#" : "\u2588";
    const std::string_view empty_char = use_ascii ? "-" : "\u2591";

    std::ranges::for_each(std::views::iota(0uz, std::size_t { config::kProgressBarWidth }),
        [&](std::size_t index) { bar_buffer += (index < filled) ? fill_char : empty_char; });

    return bar_buffer;
}

} // namespace

[[nodiscard]] std::string format_zswap_ratio(std::uint64_t uncompressed_bytes, std::uint64_t compressed_bytes) {
    if (uncompressed_bytes == 0) { return "Idle"; }
    if (compressed_bytes == 0) { return "Max"; }
    const double ratio = toDouble(uncompressed_bytes) / toDouble(compressed_bytes);
    return std::format("{:.2f}×", ratio);
}

void render_speed_results(const SpeedTestResult& result) {
    std::println("{:<{}}{:<{}}{:<{}}{:<{}}{:<{}}", " Node Name", config::kUiTableNodeWidth, "Download",
        config::kUiTableDlWidth, "Upload", config::kUiTableUlWidth, "Latency", config::kUiTableLatencyWidth, "Loss",
        config::kUiTableLossWidth);

    auto print_entry_error = [](const auto& entry) {
        std::string err = truncate_error(entry.error);
        std::print("{} {: <{}}{}Error: {}{}\n", color::kYellow, entry.node_name, config::kUiTableNodeWidth - 1,
            color::kRed, err, color::kReset);
    };

    auto print_success = [](const auto& entry) {
        std::string latency_str = (entry.latency_ms > 0.0) ? std::format("{:.2f} ms", entry.latency_ms) : "-";
        std::print("{} {: <{}}{}{:<{}}{}{:<{}}{}{:<{}}{}{:<{}}{}\n", color::kYellow, entry.node_name,
            config::kUiTableNodeWidth - 1, color::kGreen, format_speed(entry.download_mbps), config::kUiTableDlWidth,
            color::kRed, format_speed(entry.upload_mbps), config::kUiTableUlWidth, color::kCyan, latency_str,
            config::kUiTableLatencyWidth, color::kRed, entry.loss.empty() ? "-" : entry.loss, config::kUiTableLossWidth,
            color::kReset);
    };

    std::ranges::for_each(result.entries, [&](const auto& entry) {
        if (!entry.success) {
            print_entry_error(entry);
        } else {
            print_success(entry);
        }
    });
}

SpinnerCallback make_spinner_callback() {
    auto spinner = std::make_unique<UiSpinner>();
    return [spinner = std::move(spinner)](SpinnerEvent ev, std::string_view label) noexcept {
        try {
            switch (ev) {
                case SpinnerEvent::Start:
                    spinner->start(label);
                    break;
                case SpinnerEvent::Stop:
                    spinner->stop();
                    break;
            }
        } catch (...) { return; }
    };
}

std::move_only_function<void(std::size_t, std::size_t, std::string_view) const noexcept> make_progress_callback(
    std::uint8_t label_width) {

    struct State {
        std::chrono::steady_clock::time_point last_update = std::chrono::steady_clock::now();
        std::mutex mtx;
    };
    auto state = std::make_unique<State>();

    return [label_width, state = std::move(state)](
               std::size_t current, std::size_t total, std::string_view label) noexcept {
        auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(state->mtx);
        bool should_update = (current == total) || (current == 0)
            || (std::chrono::duration_cast<std::chrono::milliseconds>(now - state->last_update).count()
                >= config::kUiUpdateIntervalMs);

        if (!should_update) { return; }

        std::size_t percent = 0;
        if (total > 0) { percent = (current * 100uz) / total; }
        render_progress_line(label, percent, label_width);
        state->last_update = now;
    };
}

void render_progress_line(std::string_view label, std::size_t percent, std::uint8_t label_width) {
    percent              = std::clamp(percent, 0uz, 100uz);
    std::string_view bar = create_progress_bar_sv(percent);

    std::print("\r\x1b[2K {:<{}} [{}] {:3}%", label, label_width, bar, percent);
    std::fflush(stdout);
}

} // namespace ui

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
#include <langinfo.h>
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

template <std::size_t N> struct FixedString {
    std::array<char, N> chars {};
    consteval FixedString(const char (&str)[N]) { std::ranges::copy(str, chars.begin()); }
    [[nodiscard]] consteval std::size_t size() const noexcept { return N - 1uz; }
};

template <FixedString Pattern> struct StringMatcher {
    [[nodiscard]] static constexpr bool match(std::string_view str) noexcept {
        const std::string_view pattern_view { Pattern.chars.data(), Pattern.size() };
        constexpr auto comp = [](char lhs, char rhs) { return std::tolower(toUChar(lhs)) == rhs; };
        return std::ranges::contains_subrange(str, pattern_view, comp);
    }
};

[[nodiscard]] std::optional<std::string_view> get_env_sv(const char* name) noexcept {
    const char* val = std::getenv(name);
    if (!val || !*val) { return std::nullopt; }
    return std::string_view { val };
}

[[nodiscard]] constexpr bool match_utf8(std::string_view val) noexcept {
    return StringMatcher<"utf-8">::match(val) || StringMatcher<"utf8">::match(val);
}

[[nodiscard]] bool check_env_utf8() noexcept {
    return get_env_sv("LC_ALL")
        .or_else([] { return get_env_sv("LC_CTYPE"); })
        .or_else([] { return get_env_sv("LANG"); })
        .transform(match_utf8)
        .value_or(false);
}

[[nodiscard]] bool detect_utf8() noexcept {
    const char* codeset = nl_langinfo(CODESET);
    if (codeset != nullptr && (StringMatcher<"utf-8">::match(codeset) || StringMatcher<"utf8">::match(codeset))) {
        return true;
    }
    return check_env_utf8();
}

[[nodiscard]] bool supports_utf8() noexcept {
    static const bool supported = detect_utf8();
    return supported;
}

/**
 * @brief Thread-safe CLI progress spinner.
 *
 * This spinner uses two distinct synchronization domains:
 * 1. An atomic handshake (spinning_ and active_ via std::atomic::wait/notify) to park/resume
 *    the worker thread during idle periods.
 * 2. A mutex and condition variable (mtx_ and stop_cv_) to guard text/timestamp updates and
 *    enable interruptible, low-latency frame delays.
 *
 * These domains are kept separate because std::atomic does not support timed waits, and
 * using a mutex-guarded condition variable for the outer idle state would require holding
 * locks or checking predicates in a way that introduces spurious wakeup complexity.
 */
class UiSpinner {
    std::string text_;
    std::chrono::steady_clock::time_point start_;
    mutable std::mutex mtx_;
    std::condition_variable stop_cv_;
    std::span<const std::string_view> frames_ {};
    std::atomic<bool> spinning_ { false };
    std::atomic<bool> active_ { false };
    std::jthread worker_;

    void worker_loop(std::stop_token st) {
        std::stop_callback wake { st, [this] {
                                     spinning_.store(true, std::memory_order_release);
                                     spinning_.notify_one();
                                     stop_cv_.notify_all();
                                 } };
        std::size_t idx = 0uz;

        while (!st.stop_requested()) {
            spinning_.wait(false, std::memory_order_acquire);
            if (st.stop_requested() || !spinning_.load(std::memory_order_acquire)) { continue; }

            active_.store(true, std::memory_order_relaxed);
            scope_exit cleanup_active { [this] {
                std::print("\r\x1b[2K");
                std::fflush(stdout);
                active_.store(false, std::memory_order_release);
                active_.notify_one();
            } };

            while (spinning_.load(std::memory_order_acquire) && !st.stop_requested()) {
                const auto [snapshot, start_time] = [this] {
                    std::lock_guard lk(mtx_);
                    return std::pair { text_, start_ };
                }();
                const double elapsed
                    = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                const auto frame = frames_[idx++ % frames_.size()];
                std::print("\r {:<{}} {} {:4.1f}s", snapshot, config::kProgressBarWidth, frame, elapsed);
                std::fflush(stdout);

                std::unique_lock lk(mtx_);
                stop_cv_.wait_for(lk, std::chrono::milliseconds(config::kUiSpinnerDelayMs),
                    [this, &st] { return !spinning_.load(std::memory_order_relaxed) || st.stop_requested(); });
            }
        }
    }

public:
    UiSpinner(const UiSpinner&)            = delete;
    UiSpinner& operator=(const UiSpinner&) = delete;
    UiSpinner(UiSpinner&&)                 = delete;
    UiSpinner& operator=(UiSpinner&&)      = delete;

    UiSpinner() {
        static constexpr std::array<std::string_view, 10uz> kUtfFrames
            = { "\u280B", "\u2819", "\u2839", "\u2838", "\u283C", "\u2834", "\u2826", "\u2827", "\u2807", "\u280F" };
        static constexpr std::array<std::string_view, 4uz> kAsciiFrames = { "|", "/", "-", "\\" };

        frames_ = (config::kUiForceAscii || !supports_utf8()) ? std::span<const std::string_view>(kAsciiFrames)
                                                              : std::span<const std::string_view>(kUtfFrames);
        worker_ = std::jthread([this](std::stop_token st) { worker_loop(std::move(st)); });
    }

    ~UiSpinner() { stop(); }

    void start(std::string_view text) {
        if (spinning_.load(std::memory_order_relaxed)) { stop(); }
        {
            std::lock_guard lk(mtx_);
            text_  = text;
            start_ = std::chrono::steady_clock::now();
        }
        spinning_.store(true, std::memory_order_release);
        spinning_.notify_one();
    }

    void stop() noexcept {
        /**
         * @brief Avoid lost wakeup race conditions on the condition variable.
         */
        const bool was_spinning = [this] {
            std::lock_guard lk(mtx_);
            return spinning_.exchange(false, std::memory_order_relaxed);
        }();

        if (was_spinning) {
            stop_cv_.notify_all();
            active_.wait(true, std::memory_order_acquire);
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

    const std::size_t required_capacity = safe_mul(toSize(config::kProgressBarWidth), 3uz).value_or(0uz);
    if (bar_buffer.capacity() < required_capacity) { bar_buffer.reserve(required_capacity); }
    bar_buffer.clear();

    percent = std::clamp(percent, 0uz, 100uz);

    const std::size_t max_width = toSize(config::kProgressBarWidth);
    const std::size_t filled    = std::min(safe_mul(percent, max_width).value_or(0uz) / 100uz, max_width);

    const bool use_ascii              = config::kUiForceAscii || !supports_utf8();
    const std::string_view fill_char  = use_ascii ? "#" : "\u2588";
    const std::string_view empty_char = use_ascii ? "-" : "\u2591";

    auto append = [](std::string_view ch) { bar_buffer += ch; };
    std::ranges::for_each(std::views::repeat(fill_char, filled), append);
    std::ranges::for_each(std::views::repeat(empty_char, max_width - filled), append);

    return bar_buffer;
}

} // namespace

[[nodiscard]] std::string format_zswap_ratio(std::uint64_t uncompressed_bytes, std::uint64_t compressed_bytes) {
    if (uncompressed_bytes == 0uz) { return "Idle"; }
    if (compressed_bytes == 0uz) { return "Max"; }
    const double ratio = toDouble(uncompressed_bytes) / toDouble(compressed_bytes);
    return std::format("{:.2f}×", ratio);
}

void render_speed_results(const SpeedTestResult& result) {
    std::println("{:<{}}{:<{}}{:<{}}{:<{}}{:<{}}", " Node Name", config::kUiTableNodeWidth, "Download",
        config::kUiTableDlWidth, "Upload", config::kUiTableUlWidth, "Latency", config::kUiTableLatencyWidth, "Loss",
        config::kUiTableLossWidth);

    constexpr auto print_entry_error = [](const auto& entry) {
        const std::string err = truncate_error(entry.error);
        std::print("{} {: <{}}{}Error: {}{}\n", color::kYellow, entry.node_name, config::kUiTableNodeWidth - 1uz,
            color::kRed, err, color::kReset);
    };

    constexpr auto print_success = [](const auto& entry) {
        const std::string latency_str = (entry.latency_ms > 0.0) ? std::format("{:.2f} ms", entry.latency_ms) : "-";
        std::print("{} {: <{}}{}{:<{}}{}{:<{}}{}{:<{}}{}{:<{}}{}\n", color::kYellow, entry.node_name,
            config::kUiTableNodeWidth - 1uz, color::kGreen, format_speed(entry.download_mbps), config::kUiTableDlWidth,
            color::kRed, format_speed(entry.upload_mbps), config::kUiTableUlWidth, color::kCyan, latency_str,
            config::kUiTableLatencyWidth, color::kRed, entry.loss.empty() ? "-" : entry.loss, config::kUiTableLossWidth,
            color::kReset);
    };

    constexpr auto is_error   = [](const auto& entry) { return !entry.success; };
    constexpr auto is_success = [](const auto& entry) { return entry.success; };

    std::ranges::for_each(result.entries | std::views::filter(is_success), print_success);
    std::ranges::for_each(result.entries | std::views::filter(is_error), print_entry_error);
}

SpinnerCallback make_spinner_callback() {
    auto spinner = std::make_unique<UiSpinner>();
    return [spinner = std::move(spinner)](SpinnerEvent ev, std::string_view label) noexcept {
        switch (ev) {
            case SpinnerEvent::Start:
                spinner->start(label);
                break;
            case SpinnerEvent::Stop:
                spinner->stop();
                break;
        }
    };
}

std::move_only_function<void(std::size_t, std::size_t, std::string_view) const> make_progress_callback(
    std::uint8_t label_width) {

    struct SharedState {
        std::atomic<std::size_t> current { 0uz };
        std::atomic<std::size_t> total { 0uz };
        std::string label;
        mutable std::mutex label_mtx;
    };
    auto state = std::make_shared<SharedState>();

    std::jthread ui_thread([state, label_width](std::stop_token st) {
        scope_exit final_render { [state, label_width] {
            const std::size_t curr = state->current.load(std::memory_order_acquire);
            const std::size_t tot  = state->total.load(std::memory_order_acquire);

            if (tot == 0uz || curr < tot) {
                std::print("\r\x1b[2K");
                std::fflush(stdout);
            } else {
                const std::string lbl = [&state]() {
                    std::lock_guard lk(state->label_mtx);
                    return state->label;
                }();
                render_progress_line(lbl, 100uz, label_width);
            }
        } };

        std::size_t last_current = std::numeric_limits<std::size_t>::max();

        while (!st.stop_requested()) {
            const std::size_t curr = state->current.load(std::memory_order_acquire);
            const std::size_t tot  = state->total.load(std::memory_order_acquire);

            if (curr == last_current || tot == 0uz) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config::kUiUpdateIntervalMs));
                continue;
            }

            const std::size_t percent = safe_mul(curr, 100uz).value_or(0uz) / tot;
            const std::string lbl     = [&state]() {
                std::lock_guard lk(state->label_mtx);
                return state->label;
            }();
            render_progress_line(lbl, percent, label_width);
            last_current = curr;
            std::this_thread::sleep_for(std::chrono::milliseconds(config::kUiUpdateIntervalMs));
        }
    });

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
    percent                    = std::clamp(percent, 0uz, 100uz);
    const std::string_view bar = create_progress_bar_sv(percent);

    std::print("\r\x1b[2K {:<{}} [{}] {:3}%", label, label_width, bar, percent);
    std::fflush(stdout);
}

} // namespace ui

/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/cli_renderer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <functional>
#include <memory>
#include <print>
#include <iostream>  // Needed for std::cout and flush
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/color.hpp"
#include "include/config.hpp"
#include "include/speed_test.hpp"
#include "include/utils.hpp"  // For trim_sv

namespace CliRenderer {

namespace {

// C++23: consteval/constexpr logic where possible.
// Runtime check for UTF-8 support
bool is_utf8_term() {
    static const bool result = []() {
        // Lambda to check if environment variable contains "utf-8" or "utf8" (case-insensitive)
        auto check = [](const char* env_ptr) -> bool {
            if (!env_ptr || !*env_ptr)
                return false;

            std::string_view s(env_ptr);
            constexpr std::array<std::string_view, 2> keywords = {"utf-8", "utf8"};

            // Use std::ranges::any_of with a search
            return std::ranges::any_of(keywords, [&](std::string_view kw) {
                auto it = std::search(s.begin(), s.end(), kw.begin(), kw.end(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == b;
                });
                return it != s.end();
            });
        };

        if (check(std::getenv("LC_ALL")))
            return true;
        if (check(std::getenv("LC_CTYPE")))
            return true;
        if (check(std::getenv("LANG")))
            return true;

        return false;
    }();

    return result;
}

class UiSpinner {
    std::jthread worker_;
    std::string text_;
    std::chrono::steady_clock::time_point start_;
    // Store frames as string_views
    std::span<const std::string_view> frames_{};

   public:
    void start(std::string_view text) {
        text_ = text;
        start_ = std::chrono::steady_clock::now();

        // Use a lambda to select frames based on terminal capabilities
        auto selected = [] {
            static constexpr std::array<std::string_view, 10> utf_frames = {"\u280B",
                                                                            "\u2819",
                                                                            "\u2839",
                                                                            "\u2838",
                                                                            "\u283C",
                                                                            "\u2834",
                                                                            "\u2826",
                                                                            "\u2827",
                                                                            "\u2807",
                                                                            "\u280F"};
            static constexpr std::array<std::string_view, 4> ascii_frames = {"|", "/", "-", "\\"};

            return (Config::UI_FORCE_ASCII || !is_utf8_term())
                       ? std::span<const std::string_view>(ascii_frames)
                       : std::span<const std::string_view>(utf_frames);
        }();

        frames_ = selected;

        worker_ = std::jthread([this](std::stop_token st) {
            std::size_t idx = 0;

            while (!st.stop_requested()) {
                double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start_)
                        .count();

                // Safe access to frames
                auto frame = frames_[idx++ % frames_.size()];

                std::print("\r {:<28} {} {:4.1f}s", text_, frame, elapsed);
                // Force flush explicitly? std::print usually does line buffering or we rely on
                // logic.
                // '\r' assumes we overwrite.

                // std::flush is not strictly needed with std::print but good for interactive
                // spinners
                std::cout.flush();

                std::this_thread::sleep_for(std::chrono::milliseconds(Config::UI_SPINNER_DELAY_MS));
            }

            // Clear line on stop
            std::print("\r\x1b[2K");
            std::cout.flush();
        });
    }

    void stop() {
        // Request stop and join (jthread dtor does this, but explicit stop allows us to control
        // timing or do cleanup if needed)
        worker_.request_stop();
        if (worker_.joinable())
            worker_.join();
    }
};

}  // namespace

std::string format_speed(double mbps) {
    if (mbps >= 1000.0) {
        return std::format("{:.2f} Gbps", mbps / 1000.0);
    }
    return std::format("{:.2f} Mbps", mbps);
}

void render_speed_results(const SpeedTestResult& result) {
    std::println(
        "{:<24}{:<18}{:<18}{:<12}{:<8}", " Node Name", "Download", "Upload", "Latency", "Loss");

    for (const auto& entry : result.entries) {
        if (!entry.success) {
            std::string err = entry.error;
            // Truncate long error messages safely
            // In C++23/UTF-8 world, substr might cut multibyte char, but error messages from CLI
            // are usually ASCII. A more robust solution would be to use a proper unicode library,
            // but for now std::string is assumed.
            if (err.length() > 45) {
                // Ensure we don't end with a weird sequence if we can help it,
                // but std::string substr is byte-based.
                err = err.substr(0, 42) + "...";
            }

            std::print("{}{: <24}{}Error: {}{}\n",
                       Color::YELLOW,
                       " " + entry.node_name,
                       Color::RED,
                       err,
                       Color::RESET);
            continue;
        }

        std::string latency_str =
            (entry.latency_ms > 0.0) ? std::format("{:.2f} ms", entry.latency_ms) : "-";

        std::print("{}{: <24}{}{:<18}{}{:<18}{}{:<12}{}{:<8}{}\n",
                   Color::YELLOW,
                   " " + entry.node_name,
                   Color::GREEN,
                   format_speed(entry.download_mbps),
                   Color::RED,
                   format_speed(entry.upload_mbps),
                   Color::CYAN,
                   latency_str,
                   Color::RED,
                   entry.loss.empty() ? "-" : entry.loss,
                   Color::RESET);
    }
}

SpinnerCallback make_spinner_callback() {
    auto spinner = std::make_shared<UiSpinner>();
    return [spinner](SpinnerEvent ev, std::string_view label) {
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

std::string create_progress_bar(int percent) {
    percent = std::clamp(percent, 0, 100);
    const int filled = (percent * Config::PROGRESS_BAR_WIDTH) / 100;

    std::string bar;
    bar.reserve(Config::PROGRESS_BAR_WIDTH * 3);  // Reserve for Potential Unicode chars

    const bool use_ascii = Config::UI_FORCE_ASCII || !is_utf8_term();
    const std::string_view fill_char = use_ascii ? "#" : "\u2588";
    const std::string_view empty_char = use_ascii ? "-" : "\u2591";

    for (int j = 0; j < Config::PROGRESS_BAR_WIDTH; ++j) {
        bar += (j < filled) ? fill_char : empty_char;
    }

    return bar;
}

std::function<void(std::size_t, std::size_t, std::string_view)> make_progress_callback(
    int label_width) {
    return [label_width](std::size_t current, std::size_t total, std::string_view lbl) {
        int percent = 0;
        if (total > 0) {
            // Check for potential overflow before multiplication if size_t is huge
            // But usually current <= total.
            // Using floating point for percentage calculation
            double p = (static_cast<double>(current) / static_cast<double>(total)) * 100.0;
            percent = static_cast<int>(p);
        }
        render_progress_line(lbl, percent, label_width);
    };
}

void render_progress_line(std::string_view label, int percent, int label_width) {
    percent = std::clamp(percent, 0, 100);
    const std::string bar = create_progress_bar(percent);
    // \r to return to start of line, \x1b[2K to clear line
    std::print("\r\x1b[2K {:<{}} [{}] {:3}%", label, label_width, bar, percent);
    std::cout.flush();
}

}  // namespace CliRenderer

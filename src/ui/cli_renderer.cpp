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
#include <chrono>
#include <cstdlib>
#include <format>
#include <functional>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/color.hpp"
#include "include/config.hpp"
#include "include/speed_test.hpp"

namespace CliRenderer {

namespace {

bool check_str_utf8(const char* env) {
    if (!env || !*env)
        return false;

    std::string s(env);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });

    return s.find("utf-8") != std::string::npos || s.find("utf8") != std::string::npos;
}

bool is_utf8_term() {
    if (const char* val = std::getenv("LC_ALL"); val && *val) {
        return check_str_utf8(val);
    }

    if (const char* val = std::getenv("LC_CTYPE"); val && *val) {
        return check_str_utf8(val);
    }

    if (const char* val = std::getenv("LANG"); val && *val) {
        return check_str_utf8(val);
    }

    return false;
}

class UiSpinner {
    std::jthread worker_;
    std::string text_;
    std::chrono::steady_clock::time_point start_;
    std::span<const char* const> frames_{};

   public:
    void start(std::string_view text) {
        text_ = text;
        start_ = std::chrono::steady_clock::now();

        auto selected = [] {
            static constexpr std::array<const char*, 10> utf_frames = {"\u280B",
                                                                       "\u2819",
                                                                       "\u2839",
                                                                       "\u2838",
                                                                       "\u283C",
                                                                       "\u2834",
                                                                       "\u2826",
                                                                       "\u2827",
                                                                       "\u2807",
                                                                       "\u280F"};
            static constexpr std::array<const char*, 4> ascii_frames = {"|", "/", "-", "\\"};

            return (Config::UI_FORCE_ASCII || !is_utf8_term())
                       ? std::span<const char* const>(ascii_frames)
                       : std::span<const char* const>(utf_frames);
        }();

        frames_ = selected;

        worker_ = std::jthread([this](std::stop_token st) {
            std::size_t idx = 0;

            while (!st.stop_requested()) {
                double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start_)
                        .count();
                std::print(
                    "\r {:<28} {} {:4.1f}s", text_, frames_[idx++ % frames_.size()], elapsed);

                std::this_thread::sleep_for(std::chrono::milliseconds(Config::UI_SPINNER_DELAY_MS));
            }

            std::print("\r\x1b[2K");
        });
    }

    void stop() {
        worker_ = std::jthread();
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
            if (err.length() > 45)
                err = err.substr(0, 42) + "...";
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
    const int filled = (percent * Config::PROGRESS_BAR_WIDTH) / 100;

    std::string bar;
    bar.reserve(Config::PROGRESS_BAR_WIDTH * 3);  // Unicode chars can be 3 bytes

    const char* fill_char = (Config::UI_FORCE_ASCII || !is_utf8_term()) ? "#" : "\u2588";
    const char* empty_char = (Config::UI_FORCE_ASCII || !is_utf8_term()) ? "-" : "\u2591";

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
            percent = static_cast<int>((current * 100) / total);
            percent = std::max(0, std::min(100, percent));
        }
        render_progress_line(lbl, percent, label_width);
    };
}

void render_progress_line(std::string_view label, int percent, int label_width) {
    const std::string bar = create_progress_bar(percent);
    std::print("\r\x1b[2K {:<{}} [{}] {:3}%", label, label_width, bar, percent);
}
}  // namespace CliRenderer
#include "include/cli_renderer.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
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
            static constexpr std::array<const char*, 10> utf_frames = {
                "\u280B", "\u2819", "\u2839", "\u2838", "\u283C",
                "\u2834", "\u2826", "\u2827", "\u2807", "\u280F"
            };
            static constexpr std::array<const char*, 4> ascii_frames = {"|", "/", "-", "\\"};

            const char* lang = std::getenv("LANG");
            bool utf_ok = lang && std::string_view(lang).find("UTF-8") != std::string_view::npos;
            return utf_ok ? std::span<const char* const>(utf_frames)
                          : std::span<const char* const>(ascii_frames);
        }();

        frames_ = selected;

        worker_ = std::jthread([this](std::stop_token st) {
            std::size_t idx = 0;
            
            while (!st.stop_requested()) {
                double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
                std::print("\r {:<28} {} {:4.1f}s", text_, frames_[idx++ % frames_.size()], elapsed);
                std::cout.flush();
                
                std::this_thread::sleep_for(std::chrono::milliseconds(Config::UI_SPINNER_DELAY_MS));
            }

            std::print("\r\x1b[2K");
            std::cout.flush();
        });
    }

    void stop() {
        worker_ = std::jthread();
    }
};

}

std::string format_speed(double mbps) {
    if (mbps >= 1000.0) {
        return std::format("{:.2f} Gbps", mbps / 1000.0);
    }
    return std::format("{:.2f} Mbps", mbps);
}

void render_speed_results(const SpeedTestResult& result) {
    std::println("{:<24}{:<18}{:<18}{:<12}{:<8}", " Node Name", "Download", "Upload", "Latency", "Loss");
    for (const auto& entry : result.entries) {
        if (!entry.success) {
            std::string err = entry.error;
            if (err.length() > 45) err = err.substr(0, 42) + "...";
            std::print("{}{: <24}{}Error: {}{}\n",
                Color::YELLOW, " " + entry.node_name, Color::RED, err, Color::RESET);
            continue;
        }

        std::string latency_str = (entry.latency_ms > 0.0) 
            ? std::format("{:.2f} ms", entry.latency_ms) 
            : "-";

        std::print("{}{: <24}{}{:<18}{}{:<18}{}{:<12}{}{:<8}{}\n",
            Color::YELLOW, " " + entry.node_name,
            Color::GREEN, format_speed(entry.download_mbps),
            Color::RED,   format_speed(entry.upload_mbps),
            Color::CYAN,  latency_str,
            Color::RED,   entry.loss.empty() ? "-" : entry.loss,
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

}
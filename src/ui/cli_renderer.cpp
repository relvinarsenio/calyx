#include "include/cli_renderer.hpp"

#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "include/color.hpp"
#include "include/config.hpp"
#include "include/speed_test.hpp"

namespace CliRenderer {

namespace {

class UiSpinner {
    std::jthread worker_;
    std::string text_;

public:
    void start(std::string_view text) {
        text_ = text;

        worker_ = std::jthread([this](std::stop_token st) {
            static constexpr std::string_view frames = "|/-\\";
            std::size_t idx = 0;
            
            while (!st.stop_requested()) {
                std::print("\r{} {}", text_, frames[idx++ % frames.size()]);
                std::cout.flush();
                
                std::this_thread::sleep_for(std::chrono::milliseconds(Config::UI_SPINNER_DELAY_MS));
            }

            std::print("\r{}\r", std::string(text_.size() + 2, ' '));
            std::cout.flush();
        });
    }

    void stop() {
        worker_ = std::jthread();
    }
};

}

void render_disk_suite(const DiskSuiteResult& suite) {
    std::println("Running I/O Test (1GB File)...");
    for (const auto& run : suite.runs) {
        std::println("{}{}", run.label, Color::colorize(std::format("{:.1f} MB/s", run.mbps), Color::YELLOW));
    }
    if (!suite.runs.empty()) {
        double avg = suite.average_mbps;
        std::println(" I/O Speed (Average) : {}", Color::colorize(std::format("{:.1f} MB/s", avg), Color::YELLOW));
    }
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
            Color::CYAN,  std::format("{:.2f} ms", entry.latency_ms),
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
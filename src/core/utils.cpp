#include "include/utils.hpp"

#include <array>
#include <filesystem>
#include <format>
#include <iostream>
#include <print>
#include <system_error>
#include <vector>

#include "include/config.hpp"
#include "include/interrupts.hpp"

namespace fs = std::filesystem;

void print_line() {
    std::println("{:-<78}", "");
    std::cout << std::flush;
}

std::string format_bytes(std::uint64_t bytes) {
    if (bytes == 0) return "0";
    
    static constexpr std::array units = {"B", "KB", "MB", "GB", "TB"};
    
    int i = 0;
    double d = static_cast<double>(bytes);
    while (d >= 1024 && i < 4) {
        d /= 1024;
        i++;
    }
    return std::format("{:.1f} {}", d, units[static_cast<size_t>(i)]);
}

void cleanup_artifacts() {
    const std::vector<std::string> paths = {
        std::string(Config::SPEEDTEST_TGZ),
        "speedtest-cli",
        std::string(Config::BENCH_FILENAME)
    };
    const auto exe_dir = get_exe_dir();
    for (const auto& p : paths) {
        std::error_code ec;
        if (fs::exists(p, ec)) {
            fs::remove_all(p, ec);
        }
        auto abs_path = exe_dir / p;
        if (fs::exists(abs_path, ec)) {
            fs::remove_all(abs_path, ec);
        }
    }
}

std::filesystem::path get_exe_dir() {
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && exe.has_parent_path()) return exe.parent_path();
    return std::filesystem::current_path();
}
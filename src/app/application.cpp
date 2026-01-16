/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/application.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/cli_renderer.hpp"
#include "include/color.hpp"
#include "include/config.hpp"
#include "include/disk_benchmark.hpp"
#include "include/http_client.hpp"
#include "include/http_context.hpp"
#include "include/interrupts.hpp"
#include "include/results.hpp"
#include "include/speed_test.hpp"
#include "include/system_info.hpp"
#include "include/tgz_extractor.hpp"
#include "include/utils.hpp"

namespace fs = std::filesystem;
using namespace std::chrono;
using json = nlohmann::json;

void Application::show_help(const std::string& app_name) const {
    std::println("Usage: {}", app_name);
    std::println("");
    std::println("Options:");
    std::println("  -h, --help              Show this help message");
    std::println("  -v, --version           Show version information");
    std::println("");
    std::println("Examples:");
    std::println("  {}                   # Run VPS profiling", app_name);
}

void Application::show_version() const {
    std::println("{} v{}", Config::APP_NAME, Config::APP_VERSION);
    std::println("Copyright (c) 2025 Alfie Ardinata");
    std::println("Licensed under the Mozilla Public License 2.0");
}

int Application::run(int argc, char* argv[]) {
    try {
        SignalGuard signal_guard;
        HttpContext http_context;

        std::string app_name{Config::APP_NAME};
        if (argc > 0) {
            app_name = fs::path(argv[0]).filename().string();
            if (app_name.empty())
                app_name = Config::APP_NAME;
        }

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "-h" || arg == "--help") {
                show_help(app_name);
                return 0;
            } else if (arg == "-v" || arg == "--version") {
                show_version();
                return 0;
            } else {
                std::println(
                    stderr, "{}Error: Unknown option '{}'{}", Color::RED, arg, Color::RESET);
                show_help(app_name);
                return 1;
            }
        }

        HttpClient http;
        auto start_time = high_resolution_clock::now();

        std::print("\033c");
        print_centered_header(std::format("Calyx - Rapid VPS Profiler (v{})", Config::APP_VERSION));
        std::println(" {:<{}} : {} ({})",
                     "Author",
                     Config::APP_AUTHOR_LABEL_WIDTH,
                     "Alfie Ardinata",
                     "https://calyx.pages.dev/");
        std::println(" {:<{}} : {}",
                     "GitHub",
                     Config::APP_AUTHOR_LABEL_WIDTH,
                     "https://github.com/relvinarsenio/calyx");
        std::println(" {:<{}} : ./{}", "Usage", Config::APP_AUTHOR_LABEL_WIDTH, app_name);
        print_line();

        std::println(" -> {}", Color::colorize("CPU & Hardware", Color::BOLD));
        std::println(" {:<{}} : {}",
                     "CPU Model",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_model_name(), Color::CYAN));
        std::println(" {:<{}} : {}",
                     "CPU Cores",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_cpu_cores_freq(), Color::CYAN));
        std::println(" {:<{}} : {}",
                     "CPU Cache",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_cpu_cache(), Color::CYAN));
        std::println(" {:<{}} : {}",
                     "AES-NI",
                     Config::APP_INFO_LABEL_WIDTH,
                     SystemInfo::has_aes() ? Color::colorize("\u2713 Enabled", Color::GREEN)
                                           : Color::colorize("\u2717 Disabled", Color::RED));
        std::println(" {:<{}} : {}",
                     "VM-x/AMD-V",
                     Config::APP_INFO_LABEL_WIDTH,
                     SystemInfo::has_vmx() ? Color::colorize("\u2713 Enabled", Color::GREEN)
                                           : Color::colorize("\u2717 Disabled", Color::RED));

        std::println("\n -> {}", Color::colorize("System Info", Color::BOLD));
        std::println(" {:<{}} : {}",
                     "OS",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_os(), Color::CYAN));
        std::println(" {:<{}} : {}",
                     "Arch",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_arch(), Color::YELLOW));
        std::println(" {:<{}} : {}",
                     "Kernel",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_kernel(), Color::YELLOW));
        std::println(" {:<{}} : {}",
                     "TCP CC",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_tcp_cc(), Color::YELLOW));
        std::println(" {:<{}} : {}",
                     "Virtualization",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_virtualization(), Color::CYAN));
        std::println(" {:<{}} : {}",
                     "System Uptime",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_uptime(), Color::CYAN));
        std::println(" {:<{}} : {}",
                     "Load Average",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(SystemInfo::get_load_avg(), Color::YELLOW));

        std::error_code ec;
        std::string current_dir = fs::current_path(ec).string();
        if (ec)
            current_dir = ".";
        std::string dev_name = SystemInfo::get_device_name(current_dir);

        auto mem = SystemInfo::get_memory_status();
        auto disk = SystemInfo::get_disk_usage(current_dir);

        std::println("\n -> {}", Color::colorize("Storage & Memory", Color::BOLD));
        std::println(" {:<{}} : {} ({})",
                     "Disk Test Path",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(current_dir, Color::CYAN),
                     Color::colorize(dev_name, Color::YELLOW));
        std::println(" {:<{}} : {} ({} Used)",
                     "Total Disk",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(format_bytes(disk.total), Color::YELLOW),
                     Color::colorize(format_bytes(disk.used), Color::CYAN));
        std::println(" {:<{}} : {} ({} Used)",
                     "Total Mem",
                     Config::APP_INFO_LABEL_WIDTH,
                     Color::colorize(format_bytes(mem.total), Color::YELLOW),
                     Color::colorize(format_bytes(mem.used), Color::CYAN));

        auto swaps = SystemInfo::get_swaps();
        if (!swaps.empty()) {
            uint64_t total_swap = 0;
            uint64_t used_swap = 0;
            for (const auto& swap : swaps) {
                total_swap += swap.size;
                used_swap += swap.used;
            }

            std::println(" {:<{}} : {} ({} Used)",
                         "Total Swap",
                         Config::APP_INFO_LABEL_WIDTH,
                         Color::colorize(format_bytes(total_swap), Color::YELLOW),
                         Color::colorize(format_bytes(used_swap), Color::CYAN));

            for (const auto& swap : swaps) {
                std::string label = "   -> " + swap.type;
                if (swap.is_zswap) {
                    std::println("{:<{}} : {}",
                                 label,
                                 Config::APP_SWAP_LABEL_WIDTH,
                                 Color::colorize(swap.path, Color::GREEN));
                } else {
                    std::string info = std::format(
                        "{} ({} Used)", format_bytes(swap.size), format_bytes(swap.used));
                    std::println("{:<{}} : {} ({})",
                                 label,
                                 Config::APP_SWAP_LABEL_WIDTH,
                                 Color::colorize(info, Color::CYAN),
                                 swap.path);
                }
            }
        }

        std::println("\n -> {}", Color::colorize("Network", Color::BOLD));
        bool v4 = http.check_connectivity("ipv4.google.com");
        bool v6 = http.check_connectivity("ipv6.google.com");
        std::print(" {:<{}} : {} / {}\n",
                   "IPv4/IPv6",
                   Config::APP_INFO_LABEL_WIDTH,
                   v4 ? Color::colorize("\u2713 Online", Color::GREEN)
                      : Color::colorize("\u2717 Offline", Color::RED),
                   v6 ? Color::colorize("\u2713 Online", Color::GREEN)
                      : Color::colorize("\u2717 Offline", Color::RED));

        auto ip_res = http.get("https://speed.cloudflare.com/meta");
        if (ip_res) {
            try {
                auto data = json::parse(*ip_res);
                int asn = data.value("asn", 0);
                std::string org_name = data.value("asOrganization", "");
                std::string city = data.value("city", "-");
                std::string country = data.value("country", "-");
                std::string region = data.value("region", "");

                std::string display_isp = org_name;
                if (asn != 0 && !org_name.empty()) {
                    display_isp = std::format("AS{} {}", asn, org_name);
                }

                if (!display_isp.empty()) {
                    std::println(" {:<{}} : {}",
                                 "ISP",
                                 Config::APP_INFO_LABEL_WIDTH,
                                 Color::colorize(display_isp, Color::CYAN));
                }
                std::println(" {:<{}} : {} / {}",
                             "Location",
                             Config::APP_INFO_LABEL_WIDTH,
                             Color::colorize(city, Color::CYAN),
                             Color::colorize(country, Color::CYAN));
                if (!region.empty()) {
                    std::println(" {:<{}} : {}",
                                 "Region",
                                 Config::APP_INFO_LABEL_WIDTH,
                                 Color::colorize(region, Color::CYAN));
                }
            } catch (...) {
                std::println(" {:<{}} : {}",
                             "IP Info",
                             Config::APP_INFO_LABEL_WIDTH,
                             Color::colorize("Parse Error", Color::RED));
            }
        } else {
            std::println(" {:<{}} : {}",
                         "IP Info",
                         Config::APP_INFO_LABEL_WIDTH,
                         Color::colorize(std::format("Failed: {}", ip_res.error()), Color::RED));
        }

        print_line();

        constexpr int io_label_width = Config::IO_LABEL_WIDTH;
        std::vector<DiskIORunResult> disk_runs;
        disk_runs.reserve(Config::DISK_IO_RUNS);
        std::println("Running I/O Test (1GB File)...");

        bool disk_error = false;
        for (int i = 1; i <= Config::DISK_IO_RUNS; ++i) {
            std::string label = std::format(" I/O Speed (Run #{})", i);
            auto progress_cb = CliRenderer::make_progress_callback(io_label_width);

            auto result = DiskBenchmark::run_io_test(Config::DISK_TEST_SIZE_MB, label, progress_cb);
            std::print("\r\x1b[2K");

            if (result) {
                std::println(" {:<{}}: {}   {}",
                             result->label,
                             io_label_width,
                             Color::colorize(std::format("Write {:>8.1f} MB/s", result->write_mbps),
                                             Color::YELLOW),
                             Color::colorize(std::format("Read {:>8.1f} MB/s", result->read_mbps),
                                             Color::CYAN));
                disk_runs.push_back(*result);
            } else {
                std::println(
                    "\r{}[!] Disk Test Aborted: {}{}", Color::RED, result.error(), Color::RESET);
                disk_error = true;
                break;
            }
        }

        if (!disk_error) {
            double total_w = 0.0, total_r = 0.0;
            for (const auto& r : disk_runs) {
                total_w += r.write_mbps;
                total_r += r.read_mbps;
            }
            double avg_w =
                disk_runs.empty() ? 0.0 : total_w / static_cast<double>(disk_runs.size());
            double avg_r =
                disk_runs.empty() ? 0.0 : total_r / static_cast<double>(disk_runs.size());

            std::println(" {:<{}}: {}   {}",
                         " I/O Speed (Average)",
                         io_label_width,
                         Color::colorize(std::format("Write {:>8.1f} MB/s", avg_w), Color::YELLOW),
                         Color::colorize(std::format("Read {:>8.1f} MB/s", avg_r), Color::CYAN));

            std::println(
                "{}",
                Color::colorize(
                    "Note: Write speed reflects real disk commit speed (O_DIRECT).",
                    Color::BOLD));
        }

        print_line();

        SpeedTest st(http);
        try {
            st.install();
            auto spinner_cb = CliRenderer::make_spinner_callback();
            auto speed_result = st.run(spinner_cb);
            CliRenderer::render_speed_results(speed_result);
        } catch (const std::exception& e) {
            std::println(stderr, "\n{}Speedtest Error: {}{}", Color::RED, e.what(), Color::RESET);
        }

        print_line();
        auto end_time = high_resolution_clock::now();
        double elapsed_sec = duration<double>(end_time - start_time).count();
        if (elapsed_sec >= Config::TIME_MINUTES_THRESHOLD) {
            int minutes = static_cast<int>(elapsed_sec / Config::SECONDS_PER_MINUTE);
            double seconds =
                elapsed_sec - static_cast<double>(minutes) * Config::SECONDS_PER_MINUTE;
            std::println(" Finished in        : {} min {:.0f} sec", minutes, seconds);
        } else {
            std::println(" Finished in        : {:.0f} sec", elapsed_sec);
        }

    } catch (const std::exception& e) {
        std::println(stderr, "\n{}Fatal Error: {}{}", Color::RED, e.what(), Color::RESET);
        cleanup_artifacts();
        return 1;
    }

    cleanup_artifacts();
    return 0;
}
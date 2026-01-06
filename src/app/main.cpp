/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>

#include "include/cli_renderer.hpp"
#include "include/color.hpp"
#include "include/config.hpp"
#include "include/disk_benchmark.hpp"
#include "include/http_client.hpp"
#include "include/interrupts.hpp"
#include "include/results.hpp"
#include "include/speed_test.hpp"
#include "include/system_info.hpp"
#include "include/utils.hpp"

namespace fs = std::filesystem;
using namespace std::chrono;
using json = nlohmann::json;

class LibCurlContext {
public:
    LibCurlContext() {
        if (OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr) == 0) {
            throw std::runtime_error("Failed to initialize OpenSSL crypto library");
        }

        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            throw std::runtime_error("Failed to initialize libcurl globally");
        }
    }

    ~LibCurlContext() { curl_global_cleanup(); }

    LibCurlContext(const LibCurlContext&) = delete;
    LibCurlContext& operator=(const LibCurlContext&) = delete;
};

std::string get_device_name(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
        return "unknown device";

    std::ifstream mountinfo("/proc/self/mountinfo");
    if (!mountinfo)
        return "unknown device";

    const std::string target_dev = std::format("{}:{}", major(st.st_dev), minor(st.st_dev));

    std::string best_path_match = "unknown device";
    size_t best_path_len = 0;

    std::string exact_dev_match;

    std::string line;
    while (std::getline(mountinfo, line)) {
        std::stringstream ss(line);

        std::string id, parent, major_minor, root, mount_point;
        if (!(ss >> id >> parent >> major_minor >> root >> mount_point))
            continue;

        std::string token;
        while (ss >> token && token != "-")
            ;

        std::string fs_type, source;
        if (!(ss >> fs_type >> source))
            continue;

        auto format_output = [&](const std::string& src, const std::string& fs) {
            if (src == fs)
                return src;
            return std::format("{} ({})", src, fs);
        };

        if (major_minor == target_dev) {
            exact_dev_match = format_output(source, fs_type);
        }

        if (path.compare(0, mount_point.size(), mount_point) == 0) {
            bool valid_boundary = path.size() == mount_point.size() || mount_point == "/" ||
                                  path[mount_point.size()] == '/';

            if (valid_boundary && mount_point.size() > best_path_len) {
                best_path_len = mount_point.size();
                best_path_match = format_output(source, fs_type);
            }
        }
    }

    if (!exact_dev_match.empty())
        return exact_dev_match;

    return best_path_match;
}

struct ProgressStyle {
    std::string_view fill;
    std::string_view empty;
    int width;
};

constexpr ProgressStyle get_progress_style() {
    return ProgressStyle{"\u2588", "\u2591", Config::PROGRESS_BAR_WIDTH};
}

void run_app(std::string_view app_path) {
    HttpClient http;
    auto start_time = high_resolution_clock::now();

    std::string app_name = fs::path(app_path).filename().string();
    if (app_name.empty())
        app_name = "calyx";

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::print("\033c");
    print_centered_header("Calyx - Rapid VPS Profiler (v7.1.2)");
    std::println(" {:<18} : {}", "Author", "Alfie Ardinata (https://calyx.pages.dev/)");
    std::println(" {:<18} : {}", "GitHub", "https://github.com/relvinarsenio/calyx");
    std::println(" {:<18} : ./{}", "Usage", app_name);
    print_line();

    std::println(" -> {}", Color::colorize("CPU & Hardware", Color::BOLD));
    std::println(" {:<20} : {}", "CPU Model",
                 Color::colorize(SystemInfo::get_model_name(), Color::CYAN));
    std::println(" {:<20} : {}", "CPU Cores",
                 Color::colorize(SystemInfo::get_cpu_cores_freq(), Color::CYAN));
    std::println(" {:<20} : {}", "CPU Cache",
                 Color::colorize(SystemInfo::get_cpu_cache(), Color::CYAN));
    std::println(" {:<20} : {}", "AES-NI",
                 SystemInfo::has_aes() ? Color::colorize("\u2713 Enabled", Color::GREEN)
                                       : Color::colorize("\u2717 Disabled", Color::RED));
    std::println(" {:<20} : {}", "VM-x/AMD-V",
                 SystemInfo::has_vmx() ? Color::colorize("\u2713 Enabled", Color::GREEN)
                                       : Color::colorize("\u2717 Disabled", Color::RED));

    std::println("\n -> {}", Color::colorize("System Info", Color::BOLD));
    std::println(" {:<20} : {}", "OS", Color::colorize(SystemInfo::get_os(), Color::CYAN));
    std::println(" {:<20} : {}", "Arch", Color::colorize(SystemInfo::get_arch(), Color::YELLOW));
    std::println(" {:<20} : {}", "Kernel",
                 Color::colorize(SystemInfo::get_kernel(), Color::YELLOW));
    std::println(" {:<20} : {}", "TCP CC",
                 Color::colorize(SystemInfo::get_tcp_cc(), Color::YELLOW));
    std::println(" {:<20} : {}", "Virtualization",
                 Color::colorize(SystemInfo::get_virtualization(), Color::CYAN));
    std::println(" {:<20} : {}", "System Uptime",
                 Color::colorize(SystemInfo::get_uptime(), Color::CYAN));
    std::println(" {:<20} : {}", "Load Average",
                 Color::colorize(SystemInfo::get_load_avg(), Color::YELLOW));

    std::error_code ec;
    std::string current_dir = fs::current_path(ec).string();
    if (ec)
        current_dir = ".";
    std::string dev_name = get_device_name(current_dir);

    auto mem = SystemInfo::get_memory_status();
    auto disk = SystemInfo::get_disk_usage(current_dir);

    std::println("\n -> {}", Color::colorize("Storage & Memory", Color::BOLD));
    std::println(" {:<20} : {} ({})", "Disk Test Path", Color::colorize(current_dir, Color::CYAN),
                 Color::colorize(dev_name, Color::YELLOW));
    std::println(" {:<20} : {} ({} Used)", "Total Disk",
                 Color::colorize(format_bytes(disk.total), Color::YELLOW),
                 Color::colorize(format_bytes(disk.used), Color::CYAN));
    std::println(" {:<20} : {} ({} Used)", "Total Mem",
                 Color::colorize(format_bytes(mem.total), Color::YELLOW),
                 Color::colorize(format_bytes(mem.used), Color::CYAN));

    auto swaps = SystemInfo::get_swaps();
    if (!swaps.empty()) {
        uint64_t total_swap = 0;
        uint64_t used_swap = 0;
        for (const auto& s : swaps) {
            total_swap += s.size;
            used_swap += s.used;
        }

        std::println(" {:<20} : {} ({} Used)", "Total Swap",
                     Color::colorize(format_bytes(total_swap), Color::YELLOW),
                     Color::colorize(format_bytes(used_swap), Color::CYAN));

        for (const auto& s : swaps) {
            std::string label = "   -> " + s.type;

            if (s.is_zswap) {
                std::println("{:<22} : {}", label, Color::colorize(s.path, Color::GREEN));
            } else {
                std::string info =
                    std::format("{} ({} Used)", format_bytes(s.size), format_bytes(s.used));
                std::println("{:<22} : {} ({})", label, Color::colorize(info, Color::CYAN), s.path);
            }
        }
    }

    std::println("\n -> {}", Color::colorize("Network", Color::BOLD));
    bool v4 = http.check_connectivity("ipv4.google.com");
    bool v6 = http.check_connectivity("ipv6.google.com");
    std::print(" {:<20} : {} / {}\n", "IPv4/IPv6",
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
                std::println(" {:<20} : {}", "ISP", Color::colorize(display_isp, Color::CYAN));
            }

            std::println(" {:<20} : {} / {}", "Location", Color::colorize(city, Color::CYAN),
                         Color::colorize(country, Color::CYAN));

            if (!region.empty()) {
                std::println(" {:<20} : {}", "Region", Color::colorize(region, Color::CYAN));
            }

        } catch (...) {
            std::println(" {:<20} : {}", "IP Info", Color::colorize("Parse Error", Color::RED));
        }
    } else {
        std::println(" {:<20} : {}", "IP Info",
                     Color::colorize("Failed: " + ip_res.error(), Color::RED));
    }

    print_line();

    constexpr int io_label_width = Config::IO_LABEL_WIDTH;
    std::vector<DiskIORunResult> disk_runs;
    disk_runs.reserve(3);
    std::println("Running I/O Test (1GB File)...");

    bool disk_error = false;
    for (int i = 1; i <= Config::DISK_IO_RUNS; ++i) {
        std::string label = std::format(" I/O Speed (Run #{})", i);
        auto progress_cb = [&](std::size_t current, std::size_t total, std::string_view lbl) {
            const auto style = get_progress_style();
            int percent = static_cast<int>((current * 100) / total);
            int filled = static_cast<int>((percent * style.width) / 100);

            std::string bar;
            bar.reserve(style.width * static_cast<int>(style.fill.size()));
            for (int j = 0; j < style.width; ++j) {
                bar += (j < filled) ? style.fill : style.empty;
            }

            std::print("\r\x1b[2K {:<{}} [{}] {:3}%", lbl, io_label_width, bar, percent);
        };

        auto result = DiskBenchmark::run_io_test(Config::DISK_TEST_SIZE_MB, label, progress_cb);
        std::print("\r\x1b[2K");

        if (result) {
            std::println(
                " {:<{}}: {}   {}", result->label, io_label_width,
                Color::colorize(std::format("Write {:>8.1f} MB/s", result->write_mbps),
                                Color::YELLOW),
                Color::colorize(std::format("Read {:>8.1f} MB/s", result->read_mbps), Color::CYAN));
            disk_runs.push_back(*result);
        } else {
            std::println("\r{}[!] Disk Benchmark Aborted: {}{}", Color::RED, result.error(),
                         Color::RESET);
            disk_error = true;
            break;
        }
    }

    if (!disk_error) {
        double total_w = 0.0;
        double total_r = 0.0;
        for (const auto& r : disk_runs) {
            total_w += r.write_mbps;
            total_r += r.read_mbps;
        }
        double avg_w = disk_runs.empty() ? 0.0 : total_w / static_cast<double>(disk_runs.size());
        double avg_r = disk_runs.empty() ? 0.0 : total_r / static_cast<double>(disk_runs.size());

        std::println(" {:<{}}: {}   {}", " I/O Speed (Average)", io_label_width,
                     Color::colorize(std::format("Write {:>8.1f} MB/s", avg_w), Color::YELLOW),
                     Color::colorize(std::format("Read {:>8.1f} MB/s", avg_r), Color::CYAN));

        std::println(
            "{}",
            Color::colorize(
                "Note: Write speed reflects real disk commit speed, not temporary cache speed.",
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
    if (elapsed_sec >= 60.0) {
        int minutes = static_cast<int>(elapsed_sec / 60.0);
        double seconds = elapsed_sec - static_cast<double>(minutes) * 60.0;
        std::println(" Finished in        : {} min {:.0f} sec", minutes, seconds);
    } else {
        std::println(" Finished in        : {:.0f} sec", elapsed_sec);
    }
}

int main(int argc, char* argv[]) {
    try {
        SignalGuard signal_guard;
        LibCurlContext curl_context;

        std::string_view app_path = (argc > 0) ? argv[0] : "calyx";
        run_app(app_path);
    } catch (const std::exception& e) {
        std::println(stderr, "\n{}Fatal Error: {}{}", Color::RED, e.what(), Color::RESET);
        cleanup_artifacts();
        return 1;
    }

    cleanup_artifacts();
    return 0;
}
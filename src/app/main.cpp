#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

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
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            throw std::runtime_error("Failed to initialize libcurl globally");
        }
    }

    ~LibCurlContext() {
        curl_global_cleanup();
    }

    LibCurlContext(const LibCurlContext&) = delete;
    LibCurlContext& operator=(const LibCurlContext&) = delete;
};

void run_app(std::string_view app_path) {
    HttpClient http;
    auto start_time = high_resolution_clock::now();

    std::string app_name = fs::path(app_path).filename().string();
    if (app_name.empty()) app_name = "bench";

    std::print("\033c");
    std::cout << std::flush;
    print_line();
    std::println(" A Bench Script (C++ Edition v6.9.6)");
    std::println(" Usage : ./{}", app_name);
    print_line();

    std::println(" -> {}", Color::colorize("CPU & Hardware", Color::BOLD));
    std::println(" {:<20} : {}", "CPU Model", Color::colorize(SystemInfo::get_model_name(), Color::CYAN));
    std::println(" {:<20} : {}", "CPU Cores", Color::colorize(SystemInfo::get_cpu_cores_freq(), Color::CYAN));
    std::println(" {:<20} : {}", "CPU Cache", Color::colorize(SystemInfo::get_cpu_cache(), Color::CYAN));
    std::println(" {:<20} : {}", "AES-NI", SystemInfo::has_aes() ? Color::colorize("\u2713 Enabled", Color::GREEN) : Color::colorize("\u2717 Disabled", Color::RED));
    std::println(" {:<20} : {}", "VM-x/AMD-V", SystemInfo::has_vmx() ? Color::colorize("\u2713 Enabled", Color::GREEN) : Color::colorize("\u2717 Disabled", Color::RED));

    std::println("\n -> {}", Color::colorize("System Info", Color::BOLD));
    std::println(" {:<20} : {}", "OS", Color::colorize(SystemInfo::get_os(), Color::CYAN));
    std::println(" {:<20} : {}", "Arch", Color::colorize(SystemInfo::get_arch(), Color::YELLOW));
    std::println(" {:<20} : {}", "Kernel", Color::colorize(SystemInfo::get_kernel(), Color::YELLOW));
    std::println(" {:<20} : {}", "TCP CC", Color::colorize(SystemInfo::get_tcp_cc(), Color::YELLOW));
    std::println(" {:<20} : {}", "Virtualization", Color::colorize(SystemInfo::get_virtualization(), Color::CYAN));
    std::println(" {:<20} : {}", "System Uptime", Color::colorize(SystemInfo::get_uptime(), Color::CYAN));
    std::println(" {:<20} : {}", "Load Average", Color::colorize(SystemInfo::get_load_avg(), Color::YELLOW));

    auto mem = SystemInfo::get_memory_status();
    auto disk = SystemInfo::get_disk_usage("/");

    std::println("\n -> {}", Color::colorize("Storage & Memory", Color::BOLD));
    std::println(" {:<20} : {} ({} Used)", "Total Disk", Color::colorize(format_bytes(disk.total), Color::YELLOW), Color::colorize(format_bytes(disk.used), Color::CYAN));
    std::println(" {:<20} : {} ({} Used)", "Total Mem", Color::colorize(format_bytes(mem.total), Color::YELLOW), Color::colorize(format_bytes(mem.used), Color::CYAN));
    
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
                 std::string info = std::format("{} ({} Used)", format_bytes(s.size), format_bytes(s.used));
                 std::println("{:<22} : {} ({})", label, Color::colorize(info, Color::CYAN), s.path);
            }
        }
    }

    std::println("\n -> {}", Color::colorize("Network", Color::BOLD));
    bool v4 = http.check_connectivity("ipv4.google.com");
    bool v6 = http.check_connectivity("ipv6.google.com");
    std::print(" {:<20} : {} / {}\n", "IPv4/IPv6",
        v4 ? Color::colorize("\u2713 Online", Color::GREEN) : Color::colorize("\u2717 Offline", Color::RED),
        v6 ? Color::colorize("\u2713 Online", Color::GREEN) : Color::colorize("\u2717 Offline", Color::RED)
    );

    try {
        std::string json_str = http.get("http://ip-api.com/json");
        auto data = json::parse(json_str);

        std::string org = data.value("as", "");
        std::string city = data.value("city", "-");
        std::string country = data.value("country", "-");
        std::string region = data.value("regionName", "");

        if (!org.empty()) {
            std::println(" {:<20} : {}", "ISP", Color::colorize(org, Color::CYAN));
        }

        std::println(" {:<20} : {} / {}", "Location",
            Color::colorize(city, Color::CYAN),
            Color::colorize(country, Color::CYAN));

        if (!region.empty()) {
            std::println(" {:<20} : {}", "Region", Color::colorize(region, Color::CYAN));
        }

    } catch (const std::exception& e) {
        std::println(" {:<20} : {}", "IP Info", Color::colorize("Failed to fetch info", Color::RED));
    }

    print_line();

    std::vector<DiskRunResult> disk_runs;
    disk_runs.reserve(3);
    for(int i=1; i<=3; ++i) {
        std::string label = std::format(" I/O Speed (Run #{}) : ", i);
        auto progress_cb = [&](std::size_t current, std::size_t total, std::string_view lbl) {
            int percent = static_cast<int>((current * 100) / total);
            std::print("\r{} [{:3}%] ", lbl, percent);
            std::cout << std::flush;
        };

        auto run = DiskBenchmark::run_write_test(1024, label, progress_cb);
        std::print("\r{}\r", std::string(label.size() + 6, ' '));
        std::cout << std::flush;
        disk_runs.push_back(run);
    }

    double total_speed = 0.0;
    for (const auto& r : disk_runs) total_speed += r.mbps;
    double avg = disk_runs.empty() ? 0.0 : total_speed / static_cast<double>(disk_runs.size());
    DiskSuiteResult suite{std::move(disk_runs), avg};

    CliRenderer::render_disk_suite(suite);

    print_line();

    SpeedTest st(http);
    st.install();
    auto spinner_cb = CliRenderer::make_spinner_callback();
    auto speed_result = st.run(spinner_cb);
    CliRenderer::render_speed_results(speed_result);

    print_line();
    auto end_time = high_resolution_clock::now();
    std::println(" Finished in        : {:.0f} sec", duration<double>(end_time - start_time).count());
}

int main(int argc, char* argv[]) {
    try {
        SignalGuard signal_guard;
        LibCurlContext curl_context;
        
        std::string_view app_path = (argc > 0) ? argv[0] : "bench";
        run_app(app_path);
    } catch (const std::exception& e) {
        std::println(stderr, "\n{}Error: {}{}", Color::RED, e.what(), Color::RESET);
        cleanup_artifacts();
        return 1;
    }

    cleanup_artifacts();
    return 0;
}
/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "application.hpp"

#include "cli_renderer.hpp"
#include "color.hpp"
#include "config.hpp"
#include "disk_benchmark.hpp"
#include "http_client.hpp"
#include "http_context.hpp"
#include "interrupts.hpp"
#include "results.hpp"
#include "speed_test.hpp"
#include "system_info.hpp"
#include "utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <glaze/glaze.hpp>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;

struct NetworkMetadataRaw {
    std::uint32_t asn { 0 };
    std::string asOrganization;
    std::string city;
    std::string country;
    std::string region;
};

namespace {

constexpr std::uint8_t kCliHelp    = 1u << 0;
constexpr std::uint8_t kCliVersion = 1u << 1;

struct CliOptionSpec {
    std::string_view short_name;
    std::string_view long_name;
    std::uint8_t mask;
    std::string_view description;
};

constexpr auto kCliOptionSpecs = std::to_array<CliOptionSpec>({
    { "-h", "--help", kCliHelp, "Show this help message" },
    { "-v", "--version", kCliVersion, "Show version information" },
});

[[nodiscard]] std::uint8_t get_cli_option_mask(std::string_view arg) {
    const auto it = std::ranges::find_if(kCliOptionSpecs,
        [arg](const CliOptionSpec& option) { return option.short_name == arg || option.long_name == arg; });
    return it != kCliOptionSpecs.end() ? it->mask : std::uint8_t { 0 };
}

[[nodiscard]] std::expected<std::uint8_t, std::string_view> parse_cli_requests(std::span<const std::string_view> args) {
    const auto end_options_it = std::ranges::find(args, std::string_view { "--" });
    auto options              = std::ranges::subrange(args.begin(), end_options_it);

    const auto unknown_it
        = std::ranges::find_if(options, [](std::string_view arg) { return get_cli_option_mask(arg) == 0; });

    if (unknown_it != options.end()) { return std::unexpected(*unknown_it); }

    const auto requested = std::ranges::fold_left(options, std::uint8_t { 0 },
        [](std::uint8_t acc, std::string_view arg) { return toUByte(acc | get_cli_option_mask(arg)); });

    return requested;
}

[[nodiscard]] std::optional<std::int32_t> dispatch_cli_request(
    std::uint8_t requested, std::move_only_function<void()> on_help, std::move_only_function<void()> on_version) {
    if ((requested & kCliHelp) != 0) {
        on_help();
        return 0;
    }
    if ((requested & kCliVersion) != 0) {
        on_version();
        return 0;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view pluralize(std::int64_t value, std::string_view singular, std::string_view plural_form) {
    return value == 1 ? singular : plural_form;
}

[[nodiscard]] std::string format_elapsed_time(std::chrono::minutes minutes, std::chrono::seconds seconds) {
    const auto minute_count = toLong(minutes.count());
    const auto second_count = toLong(seconds.count());

    if (minute_count == 0) { return std::format("{} {}", second_count, pluralize(second_count, "sec", "secs")); }

    if (second_count == 0) { return std::format("{} {}", minute_count, pluralize(minute_count, "min", "mins")); }

    return std::format("{} {} {} {}", minute_count, pluralize(minute_count, "min", "mins"), second_count,
        pluralize(second_count, "sec", "secs"));
}

struct NetworkCheckResult {
    std::jthread probe_thread;
    std::future<bool> v4_future;
    std::future<bool> v6_future;
    std::future<std::expected<std::string, std::string>> ip_future;
};

struct NetworkMetadata {
    std::string isp;
    std::string city;
    std::string country;
    std::string region;
};

[[nodiscard]] NetworkCheckResult start_network_checks() {
    std::promise<bool> v4_promise;
    std::promise<bool> v6_promise;
    std::promise<std::expected<std::string, std::string>> ip_promise;

    auto v4_fut = v4_promise.get_future();
    auto v6_fut = v6_promise.get_future();
    auto ip_fut = ip_promise.get_future();

    std::jthread probe_thread([v4_p = std::move(v4_promise), v6_p = std::move(v6_promise),
                                  ip_p = std::move(ip_promise)](std::stop_token st) mutable {
        auto multi_client    = MultiHttpClient::create();
        auto ipv4_client     = HttpClient::create();
        auto ipv6_client     = HttpClient::create();
        auto metadata_client = HttpClient::create();

        auto execution_status
            = std::expected<void, std::string> {}
                  .and_then([&multi_client, &ipv4_client, &ipv6_client, &metadata_client] {
                      return (multi_client && ipv4_client && ipv6_client && metadata_client)
                          ? std::expected<void, std::string> {}
                          : std::unexpected("Network client initialization failed");
                  })
                  .and_then([&ipv4_client] { return ipv4_client->prepare_connectivity_check(config::kPingTargetIPv4); })
                  .and_then([&ipv6_client] { return ipv6_client->prepare_connectivity_check(config::kPingTargetIPv6); })
                  .and_then([&metadata_client] { return metadata_client->prepare_get(config::kUrlCloudflareMeta); })
                  .and_then([&multi_client, &ipv4_client] { return multi_client->add_handle(*ipv4_client); })
                  .and_then([&multi_client, &ipv6_client] { return multi_client->add_handle(*ipv6_client); })
                  .and_then([&multi_client, &metadata_client] { return multi_client->add_handle(*metadata_client); })
                  .and_then([&multi_client, st] {
                      if (st.stop_requested()) { return std::expected<void, std::string> {}; }
                      return multi_client->perform();
                  });

        if (execution_status) {
            v4_p.set_value(ipv4_client->get_result_void().has_value());
            v6_p.set_value(ipv6_client->get_result_void().has_value());
            ip_p.set_value(metadata_client->get_result_string());
        } else {
            v4_p.set_value(false);
            v6_p.set_value(false);
            ip_p.set_value(std::unexpected(execution_status.error()));
        }
    });

    return NetworkCheckResult { .probe_thread = std::move(probe_thread),
        .v4_future                            = std::move(v4_fut),
        .v6_future                            = std::move(v6_fut),
        .ip_future                            = std::move(ip_fut) };
}

[[nodiscard]] std::expected<NetworkMetadata, std::string> parse_network_metadata(std::string&& response) {
    NetworkMetadataRaw raw;
    auto err = glz::read<glz::opts { .error_on_unknown_keys = false }>(raw, response);
    if (err) { return std::unexpected("Parse Error"); }

    const std::string city    = raw.city.empty() ? "-" : raw.city;
    const std::string country = raw.country.empty() ? "-" : raw.country;
    const std::string isp     = (raw.asn != 0 && !raw.asOrganization.empty())
            ? std::format("AS{} {}", raw.asn, raw.asOrganization)
            : raw.asOrganization;

    return NetworkMetadata {
        .isp     = std::move(isp),
        .city    = std::move(city),
        .country = std::move(country),
        .region  = std::move(raw.region),
    };
}

void print_network_metadata(const NetworkMetadata& metadata) {
    if (!metadata.isp.empty()) {
        std::println(" {:<{}} : {}", "ISP", config::kAppInfoLabelWidth, color::colorize(metadata.isp, color::kCyan));
    }

    std::println(" {:<{}} : {} / {}", "Location", config::kAppInfoLabelWidth,
        color::colorize(metadata.city, color::kCyan), color::colorize(metadata.country, color::kCyan));

    if (!metadata.region.empty()) {
        std::println(
            " {:<{}} : {}", "Region", config::kAppInfoLabelWidth, color::colorize(metadata.region, color::kCyan));
    }
}

void print_network_metadata_error(std::string_view error) {
    std::println(" {:<{}} : {}", "IP Info", config::kAppInfoLabelWidth,
        color::colorize(std::format("Failed: {}", error), color::kRed));
}

void print_network_info(NetworkCheckResult& net_result) {
    std::println("\n -> {}", color::colorize("Network", color::kBold));

    const bool v4 = net_result.v4_future.get();
    const bool v6 = net_result.v6_future.get();
    std::print(" {:<{}} : {} / {}\n", "IPv4/IPv6", config::kAppInfoLabelWidth,
        v4 ? color::colorize("\u2713 Online", color::kGreen) : color::colorize("\u2717 Offline", color::kRed),
        v6 ? color::colorize("\u2713 Online", color::kGreen) : color::colorize("\u2717 Offline", color::kRed));

    auto metadata = net_result.ip_future.get().and_then(parse_network_metadata);
    if (!metadata) {
        print_network_metadata_error(metadata.error());
        return;
    }

    print_network_metadata(metadata.value());
}

void print_labeled_info(std::string_view label, std::string_view value, std::string_view color) {
    std::println(" {:<{}} : {}", label, config::kAppInfoLabelWidth, color::colorize(value, color));
}

void print_feature_status(std::string_view label, bool enabled) {
    std::println(" {:<{}} : {}", label, config::kAppInfoLabelWidth,
        enabled ? color::colorize("\u2713 Enabled", color::kGreen) : color::colorize("\u2717 Disabled", color::kRed));
}

void print_size_usage(std::string_view label, std::uint64_t total, std::uint64_t used) {
    std::println(" {:<{}} : {} ({} Used)", label, config::kAppInfoLabelWidth,
        color::colorize(format_bytes(total), color::kYellow), color::colorize(format_bytes(used), color::kCyan));
}

void display_cpu_section() {
    std::println(" -> {}", color::colorize("CPU & Hardware", color::kBold));
    print_labeled_info("CPU Model", SystemInfo::get_model_name(), color::kCyan);
    print_labeled_info("CPU Cores", SystemInfo::get_cpu_cores_freq(), color::kCyan);
    print_labeled_info("CPU Cache", SystemInfo::get_cpu_cache(), color::kCyan);
    print_feature_status("AES-NI", SystemInfo::has_aes());
    print_feature_status("Hardware Virt", SystemInfo::has_vmx());
}

void display_system_section() {
    std::println("\n -> {}", color::colorize("System Info", color::kBold));
    print_labeled_info("OS", SystemInfo::get_os(), color::kCyan);
    print_labeled_info("Arch", SystemInfo::get_arch(), color::kYellow);
    print_labeled_info("Kernel", SystemInfo::get_kernel(), color::kYellow);
    print_labeled_info("TCP CC", SystemInfo::get_tcp_cc(), color::kYellow);
    print_labeled_info("Virtualization", SystemInfo::get_virtualization(), color::kCyan);
    print_labeled_info("System Uptime", SystemInfo::get_uptime(), color::kCyan);
    print_labeled_info("Load Average", SystemInfo::get_load_avg(), color::kYellow);
}

std::string build_zswap_metadata(const ZSwapStats& stats, std::uint64_t total_mem) {
    const std::uint64_t max_pool_bytes = safe_mul(total_mem, stats.max_pool_percent).value_or(0uz) / 100uz;
    if (stats.zpool.empty()) {
        return std::format(
            "{}, limit: {} ({}%)", stats.compressor, format_bytes(max_pool_bytes), stats.max_pool_percent);
    }
    return std::format(
        "{}/{}, limit: {} ({}%)", stats.compressor, stats.zpool, format_bytes(max_pool_bytes), stats.max_pool_percent);
}

std::string build_zswap_info(const SwapEntry& swap, const ZSwapStats& stats, std::uint64_t total_mem) {
    const auto metadata = build_zswap_metadata(stats, total_mem);
    if (!stats.debugfs_available) { return std::format("Active [{}] (stats require root)", metadata); }

    return std::format("{} \u2192 {} ({}) [{}]", format_bytes(swap.size), format_bytes(swap.used),
        ui::format_zswap_ratio(swap.size, swap.used), metadata);
}

std::string build_zswap_secondary_info(const ZSwapStats& stats) {
    if (!stats.debugfs_available) { return {}; }

    const bool no_activity
        = (stats.written_back == 0) && (stats.reject_reclaim_fail == 0) && (stats.pool_limit_hit == 0);

    if (no_activity) { return {}; }

    const auto page_size = get_page_size();
    const auto metrics   = std::to_array<std::tuple<std::string_view, std::uint64_t, std::string_view>>(
        { { "Spilled", stats.written_back, color::kCyan }, { "Rejected", stats.reject_reclaim_fail, color::kRed },
              { "Capped", stats.pool_limit_hit, color::kRed } });

    return metrics | std::views::filter([](const auto& item) {
        return std::get<1>(item) > 0;
    }) | std::views::transform([page_size](const auto& item) {
        return std::format(" {}: {}", std::get<0>(item),
            color::colorize(format_bytes(safe_mul(std::get<1>(item), page_size).value_or(0uz)), std::get<2>(item)));
    }) | std::views::join_with(std::string_view(" "))
        | std::ranges::to<std::string>();
}

void print_regular_swap(std::string_view label, const SwapEntry& swap) {
    const auto info = std::format("{} ({} Used)", format_bytes(swap.size), format_bytes(swap.used));
    std::println("{:<{}} : {} ({})", label, config::kAppSwapLabelWidth, color::colorize(info, color::kCyan), swap.path);
}

void print_zswap(std::string_view label, const SwapEntry& swap, std::uint64_t total_mem) {
    if (!swap.zswap_stats) {
        std::println("{:<{}} : {}", label, config::kAppSwapLabelWidth, color::colorize(swap.path, color::kGreen));
        return;
    }

    const auto& stats = *swap.zswap_stats;
    const auto info   = build_zswap_info(swap, stats, total_mem);
    std::println("{:<{}} : {}", label, config::kAppSwapLabelWidth, color::colorize(info, color::kGreen));

    const auto secondary_info = build_zswap_secondary_info(stats);
    if (!secondary_info.empty()) { std::println(" {:<{}} {}", "", config::kAppSwapLabelWidth, secondary_info); }
}

void print_swap_entry(const SwapEntry& swap, std::uint64_t total_mem) {
    const auto label = std::format("   -> {}", swap.type);
    if (swap.is_zswap) {
        print_zswap(label, swap, total_mem);
        return;
    }
    print_regular_swap(label, swap);
}

void display_storage_memory() {
    const std::string current_dir = []() {
        std::error_code ec;
        auto current_path = fs::current_path(ec);
        return ec ? "." : current_path.string();
    }();

    const auto dev_name = SystemInfo::get_device_name(current_dir);
    const auto mem      = SystemInfo::get_memory_status();
    const auto disk     = SystemInfo::get_disk_usage(current_dir);

    std::println("\n -> {}", color::colorize("Storage & Memory", color::kBold));
    std::println(" {:<{}} : {} ({})", "Test Path", config::kAppInfoLabelWidth,
        color::colorize(current_dir, color::kCyan), color::colorize(dev_name, color::kYellow));

    print_size_usage("Size Partition", disk.total, disk.used);
    print_size_usage("Total Mem", mem.total, mem.used);

    auto swaps = SystemInfo::get_swaps();
    if (swaps.empty()) { return; }

    const auto [total_swap, used_swap] = std::ranges::fold_left(
        swaps, std::pair { std::uint64_t { 0 }, std::uint64_t { 0 } }, [](auto acc, const auto& swap) {
            if (swap.is_zswap) { return acc; }
            return std::pair { safe_add(acc.first, swap.size).value_or(0uz),
                safe_add(acc.second, swap.used).value_or(0uz) };
        });

    print_size_usage("Total Swap", total_swap, used_swap);

    for (const auto& swap : swaps) {
        print_swap_entry(swap, mem.total);
    }
}

/**
 * @brief Computes stability-weighted average throughput across benchmark runs.
 *
 * Uses the Coefficient of Variation (σ/μ) from each run's latency histogram as a quality
 * signal. Runs with lower CV (tighter latency distribution) receive proportionally higher
 * weight, reducing the influence of runs contaminated by system interference.
 *
 * Weight is capped at kMaxWeight to prevent a single low-CV run from dominating the
 * average — a Winsorization strategy standard in meta-analysis (Cochran, 1954).
 *
 * @param runs   Collected benchmark run results.
 * @param phase  Pointer-to-member selecting write or read metrics from each run.
 * @return Weighted average throughput in bytes/second.
 */
[[nodiscard]] double weighted_avg_throughput(
    std::span<const DiskIORunResult> runs, const DiskIOMetrics DiskIORunResult::* phase) {
    const auto [weighted_bw, total_weight] = std::ranges::fold_left(
        runs, std::pair { 0.0, 0.0 }, [phase](auto acc, const DiskIORunResult& run) {
            static constexpr double kCvFloor    = 0.01;
            static constexpr double kMaxWeight  = 10.0;
            static constexpr double kUnitWeight = 1.0;
            const auto& metrics                 = run.*phase;
            const double stability_weight       = std::min(kUnitWeight / std::max(metrics.cv, kCvFloor), kMaxWeight);
            return std::pair { acc.first + metrics.bw_bytes_per_sec * stability_weight, acc.second + stability_weight };
        });

    return (total_weight > 0.0) ? weighted_bw / total_weight : 0.0;
}

[[nodiscard]] std::expected<DiskIORunResult, std::string> run_single_disk_benchmark(std::uint32_t run_number) {
    const auto label = std::format(" I/O Speed (Run #{})", run_number);

    scope_exit clear_line { [] {
        std::print("\r\x1b[2K");
        std::fflush(stdout);
    } };

    const auto progress_cb = ui::make_progress_callback(config::kIoLabelWidth);

    DiskBenchmark::BenchmarkConfig io_config {
        .size_mb           = config::kDiskTestSizeMb,
        .write_block_size  = config::kIoWriteBlockSize,
        .read_block_size   = config::kIoReadBlockSize,
        .write_queue_depth = std::max(std::uint16_t { 1 }, config::kIoWriteQueueDepth),
        .read_queue_depth  = std::max(std::uint16_t { 1 }, config::kIoReadQueueDepth),
        .alignment         = config::kIoAlignment,
        .label             = label,
    };

    const auto result
        = DiskBenchmark::run_io_test(io_config, progress_cb, {}, []() noexcept { return check_interrupted(); });

    if (!result) { return std::unexpected(result.error()); }

    return *result;
}

std::expected<void, std::string> run_disk_benchmarks() {
    std::vector<DiskIORunResult> disk_runs;
    disk_runs.reserve(config::kDiskIoRuns);

    const auto to_mbps = [](const DiskIOMetrics& metrics) { return metrics.bw_bytes_per_sec / (1024.0 * 1024.0); };

    std::println("Running I/O Test ({} File)...", format_bytes(toSize(config::kDiskTestSizeMb) * 1024ULL * 1024ULL));

    for (std::uint32_t run_number : std::views::iota(1u, toUInt(config::kDiskIoRuns) + 1u)) {
        const auto result = run_single_disk_benchmark(run_number);
        if (!result) { return std::unexpected(result.error()); }

        std::println(" {:<{}}:  {}   {}", result->label, config::kIoLabelWidth,
            color::colorize(std::format("Write {:>8.1f} MB/s", to_mbps(result->write)), color::kYellow),
            color::colorize(std::format("Read {:>8.1f} MB/s", to_mbps(result->read)), color::kCyan));

        disk_runs.push_back(*result);
    }

    const double avg_write_bps = weighted_avg_throughput(disk_runs, &DiskIORunResult::write);
    const double avg_read_bps  = weighted_avg_throughput(disk_runs, &DiskIORunResult::read);

    constexpr double kBytesToMiB = 1.0 / (1024.0 * 1024.0);
    std::println(" {:<{}}:  {}   {}", " I/O Speed (Average)", config::kIoLabelWidth,
        color::colorize(std::format("Write {:>8.1f} MB/s", avg_write_bps * kBytesToMiB), color::kYellow),
        color::colorize(std::format("Read {:>8.1f} MB/s", avg_read_bps * kBytesToMiB), color::kCyan));

    return {};
}

std::expected<void, std::string> run_speed_test(HttpClient& http) {
    return SpeedTest::create(http)
        .and_then([](SpeedTest st) -> std::expected<SpeedTest, std::string> {
            return st.install().transform([owned_st = std::move(st)]() mutable { return std::move(owned_st); });
        })
        .transform([](SpeedTest st) {
            auto spinner_cb   = ui::make_spinner_callback();
            auto speed_result = st.run(spinner_cb);
            ui::render_speed_results(speed_result);
        })
        .or_else([](const std::string& err) -> std::expected<void, std::string> { return std::unexpected(err); });
}

} // anonymous namespace

void Application::show_help(std::string_view app_name) const {
    std::println("Usage: {}", app_name);
    std::println("");
    std::println("Options:");
    for (const auto& option : kCliOptionSpecs) {
        auto names = std::format("{}, {}", option.short_name, option.long_name);
        std::println("  {:<24} {}", names, option.description);
    }
    std::println("");
    std::println("Examples:");
    std::println("  {}                   # Run VPS profiling", app_name);
}

void Application::show_version() const {
    std::println("{} v{}", config::kAppName, config::kAppVersion);
    std::println("Copyright (c) 2025-2026 Alfie Ardinata");
    std::println("Licensed under the Mozilla Public License 2.0");
}

std::expected<void, std::string> Application::run(int argc, char* argv[]) {
    SignalGuard signal_guard;
    auto http_context = HttpContext::create();
    if (!http_context) {
        return std::unexpected(std::format("\n[!] HttpContext create failed: {}", http_context.error()));
    }

    const std::string app_name = [argc, argv]() {
        if (argc <= 0) { return std::string { config::kAppName }; }
        auto binary_filename = fs::path(argv[0]).filename().string();
        return binary_filename.empty() ? std::string { config::kAppName } : binary_filename;
    }();

    const auto arg_count = toSize(argc >= 0 ? argc : 0);

    auto args = std::span(argv, arg_count) | std::views::drop(1)
        | std::views::transform([](const char* s) { return std::string_view(s); })
        | std::ranges::to<std::vector<std::string_view>>();

    const auto cli_parse = parse_cli_requests(std::span<const std::string_view> { args });
    if (!cli_parse) {
        show_help(app_name);
        return std::unexpected(std::format("[!] Unknown option: {}", cli_parse.error()));
    }

    const auto requested = cli_parse.value();

    const auto cli_exit
        = dispatch_cli_request(requested, [this, &app_name]() { show_help(app_name); }, [this]() { show_version(); });

    if (cli_exit.has_value()) { return {}; }

    scope_exit cleanup_guard { []() noexcept { cleanup_artifacts(); } };

    auto http = HttpClient::create();
    if (!http) { return std::unexpected(std::format("\n[!] HttpClient create failed: {}", http.error())); }
    auto start_time = high_resolution_clock::now();

    std::println("\x1b[H\x1b[2J\x1b[3J");
    print_centered_header(std::format("Calyx - Linux System Benchmarking Utility (v{})", config::kAppVersion));
    std::println(" {:<{}} : {} ({})", "Author", config::kAppAuthorLabelWidth, "Alfie Ardinata", config::kUrlMaintainer);
    std::println(" {:<{}} : {}", "GitHub", config::kAppAuthorLabelWidth, config::kUrlGithub);
    std::println(" {:<{}} : ./{}", "Usage", config::kAppAuthorLabelWidth, app_name);
    print_line();
    /**
     * @note Start network probes early to overlap remote HTTP latency with local
     *       system section rendering (CPU/OS/storage), reducing total wall time.
     */
    auto net_checks = start_network_checks();

    display_cpu_section();
    display_system_section();
    display_storage_memory();

    print_network_info(net_checks);

    print_line();

    /**
     * @note Disk benchmark failure is intentionally non-fatal so network speed
     *       testing remains available even in permission-restricted or
     *       constrained storage environments.
     */
    if (const auto res = run_disk_benchmarks(); !res) {
        print_error(std::format("\n[!] Disk Test Aborted: {}", res.error()));
    }

    print_line();

    if (auto res = run_speed_test(*http); !res) {
        print_error(std::format("\n[!] Speed Test Aborted: {}", res.error()));
    }

    print_line();
    const auto final_elapsed = [start_time]() {
        const auto total_duration = std::chrono::round<std::chrono::seconds>(high_resolution_clock::now() - start_time);
        const auto minutes_part   = std::chrono::floor<std::chrono::minutes>(total_duration);
        const auto seconds_part   = total_duration - minutes_part;
        return format_elapsed_time(minutes_part, seconds_part);
    }();

    std::println(" Finished in        : {}", final_elapsed);
    return {};
}
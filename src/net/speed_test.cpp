/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "speed_test.hpp"

#include "cli_renderer.hpp"
#include "color.hpp"
#include "config.hpp"
#include "file_descriptor.hpp"
#include "http_client.hpp"
#include "interrupts.hpp"
#include "results.hpp"
#include "scope.hpp"
#include "shell_pipe.hpp"
#include "system_info.hpp"
#include "tgz_extractor.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <glaze/glaze.hpp>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

using namespace std::chrono;
namespace fs = std::filesystem;

struct SpeedTestBandwidth {
    double bandwidth { 0.0 };
};

struct SpeedTestPing {
    double latency { 0.0 };
};

struct SpeedTestResultJson {
    std::string_view type;
    std::string_view error;
    std::string_view level;
    std::string_view message;

    std::optional<SpeedTestBandwidth> download;
    std::optional<SpeedTestBandwidth> upload;
    std::optional<SpeedTestPing> ping;
    std::optional<double> packetLoss;
};

namespace st_impl {

struct Node {
    std::string_view id;
    std::string_view name;
};

constexpr std::array<Node, 7> kServers = { { { "", "Speedtest.net (Auto)" }, { "59016", "Singapore, SG" },
    { "5905", "Los Angeles, US" }, { "59219", "Montreal, CA" }, { "40788", "London, UK" }, { "3386", "Amsterdam, NL" },
    { "12492", "Sydney, AU" } } };

constexpr std::string_view kRateLimitError = "Rate Limit Reached";

/**
 * Speedtest CLI returns bandwidth as bytes/s (JSON -f json output).
 * Formula: Mbps = (bytes/s × 8) / 1,000,000
 * Source: https://www.speedtest.net/apps/cli
 */
constexpr double kBitsPerByte = 8.0;
constexpr double kBitsPerMbps = 1'000'000.0;

std::expected<std::string, std::error_code> write_cert_file(const fs::path& dir, std::span<const std::byte> data) {
    fs::path cert_path = dir / "cacert.pem";

    return posix::file::open(cert_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, S_IRUSR | S_IWUSR)
        .and_then([&cert_path, data](posix::file file) -> std::expected<std::string, std::error_code> {
            const auto path_str = cert_path.string();
            return file.write_exact(std::as_bytes(data))
                .transform_error([](auto fail) { return fail.error; })
                .and_then([&file](auto) { return file.sync(); })
                .transform([p_str = std::move(path_str)]() mutable { return p_str; });
        });
}

[[nodiscard]] constexpr std::string_view strip_leading_error_prefix(std::string_view sv) noexcept {
    constexpr string_utils::FixedString kPattern = "error: ";
    if (string_utils::starts_with_ic<kPattern>(sv)) { return trim_sv(sv.substr(kPattern.size())); }
    return sv;
}

[[nodiscard]] std::string sanitize_error(std::string_view msg) {
    using namespace std::string_view_literals;

    const auto clean_msg = strip_leading_error_prefix(string_utils::strip_bracketed_prefix(trim_sv(msg)));

    return clean_msg | std::views::transform([](char c) { return c == '\n' ? ' ' : c; })
        | std::views::filter([](char c) { return !"[]"sv.contains(c); }) | std::ranges::to<std::string>();
}

enum class LineParseAction : std::uint8_t {
    Continue,
    FoundResult,
    RateLimited
};

struct ParsedMetrics {
    double download_mbps { 0.0 };
    double upload_mbps { 0.0 };
    double latency_ms { 0.0 };
    std::string loss { "-" };
};

struct LineParseResult {
    LineParseAction action { LineParseAction::Continue };
    std::optional<std::string> error;
    std::optional<ParsedMetrics> metrics;

    static LineParseResult make_continue(std::optional<std::string> err = std::nullopt) {
        return { LineParseAction::Continue, std::move(err), std::nullopt };
    }
    static LineParseResult make_rate_limited() {
        return { LineParseAction::RateLimited, std::string { kRateLimitError }, std::nullopt };
    }
    static LineParseResult make_found(ParsedMetrics m) {
        return { LineParseAction::FoundResult, std::nullopt, std::move(m) };
    }
};

LineParseResult process_speed_test_line(std::string_view sv) {
    if (sv.contains("Limit reached") || sv.contains("Too many requests")) {
        return LineParseResult::make_rate_limited();
    }

    SpeedTestResultJson parsed;
    if (glz::read<glz::opts { .error_on_unknown_keys = false }>(parsed, sv)) {
        return LineParseResult::make_continue();
    }

    if (!parsed.error.empty()) { return LineParseResult::make_continue(sanitize_error(parsed.error)); }

    if (parsed.type == "result" && (!parsed.download || !parsed.upload || !parsed.ping)) {
        return LineParseResult::make_continue("Malformed result (missing speed data)");
    }

    if (parsed.type == "result") {
        ParsedMetrics metrics {
            .download_mbps = parsed.download->bandwidth * kBitsPerByte / kBitsPerMbps,
            .upload_mbps   = parsed.upload->bandwidth * kBitsPerByte / kBitsPerMbps,
            .latency_ms    = parsed.ping->latency,
            .loss
            = parsed.packetLoss.transform([](double loss) { return std::format("{:.2f} %", loss); }).value_or("-"),
        };
        return LineParseResult::make_found(std::move(metrics));
    }

    if (parsed.type == "log" && parsed.level == "error") {
        const auto msg = parsed.message.empty() ? "Unknown error" : parsed.message;

        if (msg.contains("Limit reached")) { return LineParseResult::make_rate_limited(); }

        if (msg.contains("No servers defined")) { return LineParseResult::make_continue("Server Offline/Changed"); }

        return LineParseResult::make_continue(sanitize_error(msg));
    }

    return LineParseResult::make_continue();
}

enum class NodeExecutionAction : std::uint8_t {
    Continue,
    Break,
    Return,
};

struct NodeExecutionResult {
    SpeedEntryResult entry;
    NodeExecutionAction action { NodeExecutionAction::Continue };
};

struct NodeRunContext {
    const std::string& cli_path;
    std::string_view cert_path;
};

std::vector<std::string> build_speedtest_command_args(
    const std::string& cli_path, std::string_view cert_path, std::string_view server_id) {
    std::vector<std::string> cmd_args { cli_path, "-f", "json", "--accept-license", "--accept-gdpr",
        std::format("--ca-certificate={}", cert_path) };

    if (!server_id.empty()) { cmd_args.emplace_back(std::format("--server-id={}", server_id)); }

    return cmd_args;
}

struct ParseSummary {
    bool found_result = false;
    bool rate_limited = false;
    std::string error;
    std::optional<ParsedMetrics> metrics;
    std::string_view last_raw_output;
};

ParseSummary parse_speedtest_output_lines(std::string_view output) {
    ParseSummary summary;
    for (const auto line_rng : output | std::views::split('\n')) {
        std::string_view sv(line_rng);
        if (trim_sv(sv).empty()) { continue; }

        summary.last_raw_output = sv;

        const auto parse_res = process_speed_test_line(sv);

        if (parse_res.error && summary.error.empty()) { summary.error = std::move(*parse_res.error); }

        if (parse_res.action == LineParseAction::FoundResult) {
            summary.found_result = true;
            summary.metrics      = std::move(parse_res.metrics);
            break;
        }
        if (parse_res.action == LineParseAction::RateLimited) {
            summary.rate_limited = true;
            break;
        }
    }
    return summary;
}

struct FallbackErrorParams {
    ShellPipeStatus status;
    std::string_view pipe_error;
    std::int32_t pipe_exit_code;
    std::string_view last_raw_output;
};

std::string determine_fallback_error(FallbackErrorParams params) {
    switch (params.status) {
        case ShellPipeStatus::timed_out:
            return "Process timed out";
        case ShellPipeStatus::signaled:
            return "Process terminated by signal";
        case ShellPipeStatus::error:
            return params.pipe_error.empty() ? "Execution error occurred" : std::string(params.pipe_error);
        case ShellPipeStatus::success:
        case ShellPipeStatus::nonzero_exit:
        case ShellPipeStatus::interrupted:
            break;
    }

    if (!params.pipe_error.empty()) { return std::string(params.pipe_error); }

    if (params.pipe_exit_code != 0) { return std::format("Process failed with code {}", params.pipe_exit_code); }

    return !params.last_raw_output.empty()
        ? std::format("CLI Error: {}", truncate_error(trim_sv(params.last_raw_output)))
        : "No Result Data (Empty Output)";
}

NodeExecutionResult run_speed_test_for_node(const Node& node, NodeRunContext context) {
    ui::ScopedSpinner spinner(node.name);

    const auto cmd_args = build_speedtest_command_args(context.cli_path, context.cert_path, node.id);

    NodeExecutionResult node_result;
    node_result.entry.server_id = std::string(node.id);
    node_result.entry.node_name = std::string(node.name);

    auto pipe = ShellPipe::create(cmd_args);
    if (!pipe) {
        node_result.entry.success = false;
        node_result.entry.error   = std::format("Failed to run speedtest: {}", pipe.error().message());
        node_result.action        = NodeExecutionAction::Break;
        return node_result;
    }

    const auto pipe_res = pipe->read_all(
        config::kSpeedtestDlTimeout + seconds(15), {}, []() noexcept { return check_interrupted(); }, false);
    if (pipe_res.status == ShellPipeStatus::interrupted || check_interrupted()) {
        node_result.entry.success = false;
        node_result.entry.error   = std::string { config::kInterruptMsg };
        node_result.action        = NodeExecutionAction::Break;
        return node_result;
    }

    const std::string& output = pipe_res.output;

    const auto summary = parse_speedtest_output_lines(output);

    node_result.entry.rate_limited = summary.rate_limited;

    if (summary.rate_limited) {
        node_result.entry.error = std::string { kRateLimitError };
        node_result.action      = NodeExecutionAction::Return;
        return node_result;
    }

    if (summary.found_result && summary.metrics) {
        node_result.entry.success       = true;
        node_result.entry.download_mbps = summary.metrics->download_mbps;
        node_result.entry.upload_mbps   = summary.metrics->upload_mbps;
        node_result.entry.latency_ms    = summary.metrics->latency_ms;
        node_result.entry.loss          = std::move(summary.metrics->loss);
    } else {
        node_result.entry.success = false;
        node_result.entry.error   = std::move(summary.error);
    }

    if (!summary.found_result && node_result.entry.error.empty()) {
        node_result.entry.error = determine_fallback_error({
            .status          = pipe_res.status,
            .pipe_error      = pipe_res.error,
            .pipe_exit_code  = pipe_res.exit_code,
            .last_raw_output = summary.last_raw_output,
        });
    }

    return node_result;
}

std::expected<std::string_view, SpeedTestError> resolve_speedtest_url_arch(std::string_view arch) {
    using Result = std::expected<std::string_view, std::monostate>;

    return Result(std::unexpected(std::monostate {}))
        .or_else([arch](auto) -> Result {
            static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kKnownArchs
                = { { { "x86_64", "x86_64" }, { "i386", "i386" }, { "i686", "i386" }, { "i586", "i386" },
                    { "aarch64", "aarch64" }, { "arm64", "aarch64" } } };
            const auto it = std::ranges::find(kKnownArchs, arch, &std::pair<std::string_view, std::string_view>::first);
            return it != kKnownArchs.end() ? Result(it->second) : std::unexpected(std::monostate {});
        })
        .or_else([arch](auto) -> Result {
            return (arch.starts_with("armv7") || arch.starts_with("armv8l")) ? Result("armhf")
                                                                             : std::unexpected(std::monostate {});
        })
        .or_else([arch](auto) -> Result {
            return (arch.starts_with("armv6") || arch.starts_with("armv5")) ? Result("armel")
                                                                            : std::unexpected(std::monostate {});
        })
        .transform_error([&arch](auto) -> SpeedTestError {
            return SpeedTestLogicError { std::format("Unsupported architecture: {}", arch) };
        });
}

std::string build_speedtest_download_url(std::string_view url_arch) {
    return std::format("https://install.speedtest.net/app/cli/ookla-speedtest-{}-linux-{}.tgz",
        config::kSpeedtestCliVersion, url_arch);
}

} // namespace st_impl

[[nodiscard]] std::string get_error_string(const SpeedTestError& err) {
    return std::visit(
        overloaded { [](const posix::SysCallError& e) -> std::string { return format_sys_error(e.ec, e.context); },
            [](const archive::ExtractError& e) -> std::string {
                return std::string { archive::TgzExtractor::error_string(e) };
            },
            [](const HttpError& e) -> std::string { return e.message; },
            [](const SpeedTestLogicError& e) -> std::string { return e.message; } },
        err);
}

SpeedTest::SpeedTest(HttpClient& http, const std::filesystem::path& base_dir)
    : http_(http)
    , base_dir_(base_dir) {
    fs::path cli_rel(config::kSpeedtestCliPath);
    cli_dir_  = base_dir_ / cli_rel.parent_path();
    cli_path_ = base_dir_ / cli_rel;
    tgz_path_ = base_dir_ / config::kSpeedtestTgz;
}

std::expected<SpeedTest, SpeedTestError> SpeedTest::create(HttpClient& client) {
    return posix::make_temp_dir((fs::temp_directory_path() / "calyx_XXXXXX").string())
        .transform([&client](std::string dir) { return SpeedTest(client, std::move(dir)); })
        .transform_error([](std::error_code ec) -> SpeedTestError {
            return posix::SysCallError { ec, "Failed to create secure temp dir" };
        });
}

SpeedTest::~SpeedTest() {
    if (!base_dir_.empty()) {
        std::error_code ec;
        fs::remove_all(base_dir_, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            print_warning(std::format("Failed to clean up temp dir '{}': {}", base_dir_.string(), ec.message()));
        }
    }
}

std::expected<void, SpeedTestError> SpeedTest::install() {
    std::println("Downloading Speedtest CLI...");

    return st_impl::resolve_speedtest_url_arch(SystemInfo::get_raw_arch())
        .transform([](std::string_view url_arch) { return st_impl::build_speedtest_download_url(url_arch); })
        .and_then([this](const std::string& url) -> std::expected<void, SpeedTestError> {
            return http_.download(url, tgz_path_).transform_error([](const std::string& err) -> SpeedTestError {
                return HttpError { err };
            });
        })
        .and_then([this]() -> std::expected<void, SpeedTestError> {
            std::error_code ec;
            fs::create_directories(cli_dir_, ec);
            return ec ? std::unexpected<SpeedTestError>(posix::SysCallError { ec, "Installation Directory" })
                      : std::expected<void, SpeedTestError> {};
        })
        .and_then([this]() -> std::expected<void, SpeedTestError> {
            return archive::TgzExtractor::extract(tgz_path_, cli_dir_)
                .transform_error([](archive::ExtractError err) -> SpeedTestError { return err; });
        })
        .and_then([this]() -> std::expected<void, SpeedTestError> {
            return fs::exists(cli_path_) ? std::expected<void, SpeedTestError> {}
                                         : std::unexpected<SpeedTestError>(
                                               SpeedTestLogicError { "Speedtest binary not found after extraction!" });
        })
        .and_then([this]() -> std::expected<void, SpeedTestError> {
            std::error_code ec;
            fs::permissions(cli_path_, fs::perms::owner_all, fs::perm_options::add, ec);
            return ec ? std::unexpected<SpeedTestError>(posix::SysCallError { ec, "Permissions Update" })
                      : std::expected<void, SpeedTestError> {};
        });
}

SpeedTestResult SpeedTest::run() {
    SpeedTestResult result;
    result.entries.reserve(st_impl::kServers.size());

    const auto cert_expected = st_impl::write_cert_file(base_dir_, curl::get_embedded_cert());

    if (!cert_expected) {
        SpeedEntryResult entry;
        entry.node_name = "System Error";
        entry.error     = format_sys_error(cert_expected.error(), "Certificate Error");
        entry.success   = false;
        result.entries.push_back(std::move(entry));
        return result;
    }

    const std::string& cert_path = *cert_expected;
    scope_exit cert_cleanup { [&cert_path]() noexcept {
        std::error_code ec;
        fs::remove(cert_path, ec);
    } };

    const std::string cli_path = cli_path_.string();

    for (const auto& node : st_impl::kServers) {
        if (check_interrupted()) { break; }

        auto node_result = st_impl::run_speed_test_for_node(node,
            st_impl::NodeRunContext {
                .cli_path  = cli_path,
                .cert_path = cert_path,
            });

        result.entries.push_back(std::move(node_result.entry));

        if (node_result.action == st_impl::NodeExecutionAction::Return) {
            result.rate_limited = node_result.entry.rate_limited;
            return result;
        }
        if (node_result.action == st_impl::NodeExecutionAction::Break) { break; }
    }
    return result;
}

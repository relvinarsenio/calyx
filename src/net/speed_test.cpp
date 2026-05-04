/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "speed_test.hpp"

#include "color.hpp"
#include "config.hpp"
#include "file_descriptor.hpp"
#include "http_client.hpp"
#include "interrupts.hpp"
#include "results.hpp"
#include "shell_pipe.hpp"
#include "system_info.hpp"
#include "tgz_extractor.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <glaze/glaze.hpp>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/utsname.h>
#include <system_error>
#include <unistd.h>
#include <vector>

extern unsigned char cacert_pem[];
extern unsigned int cacert_pem_len;

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

namespace {

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

void mark_rate_limited(SpeedEntryResult& entry, SpeedTestResult& result) {
    entry.rate_limited  = true;
    entry.error         = std::string { kRateLimitError };
    result.rate_limited = true;
}

class ScopedCertFile {
    fs::path path_;
    std::string cached_path_;

    explicit ScopedCertFile(fs::path path, std::string path_str)
        : path_(std::move(path))
        , cached_path_(std::move(path_str)) {}

public:
    ScopedCertFile(ScopedCertFile&& other) noexcept
        : path_(std::move(other.path_))
        , cached_path_(std::move(other.cached_path_)) {
        other.path_.clear();
        other.cached_path_.clear();
    }

    ScopedCertFile& operator=(ScopedCertFile&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_        = std::move(other.path_);
            cached_path_ = std::move(other.cached_path_);
            other.path_.clear();
            other.cached_path_.clear();
        }
        return *this;
    }

    ScopedCertFile(const ScopedCertFile&)            = delete;
    ScopedCertFile& operator=(const ScopedCertFile&) = delete;

    ~ScopedCertFile() noexcept { cleanup(); }

    static std::expected<ScopedCertFile, std::error_code> create(
        const fs::path& dir, std::span<const unsigned char> data) {
        fs::path cert_path = dir / "cacert.pem";

        return posix::file::open(cert_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, S_IRUSR | S_IWUSR)
            .and_then([&cert_path, data](posix::file file) -> std::expected<ScopedCertFile, std::error_code> {
                auto path_str = cert_path.string();
                return file.write_exact(std::as_bytes(data))
                    .transform_error([](auto fail) { return fail.error; })
                    .and_then([&file](auto) { return file.sync(); })
                    .transform([path = std::move(cert_path), p_str = std::move(path_str)]() mutable {
                        return ScopedCertFile(std::move(path), std::move(p_str));
                    });
            });
    }

    [[nodiscard]] std::string_view get_path() const noexcept { return cached_path_; }

private:
    void cleanup() noexcept {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }
};

class SpinnerScope {
    SpinnerCallback& cb_;
    std::string_view label_;
    bool active_ = false;

public:
    SpinnerScope(SpinnerCallback& cb, std::string_view label)
        : cb_(cb)
        , label_(label) {
        active_ = static_cast<bool>(cb_);
        if (active_) { cb_(SpinnerEvent::Start, label_); }
    }

    ~SpinnerScope() noexcept {
        if (active_) { cb_(SpinnerEvent::Stop, label_); }
    }

    SpinnerScope(const SpinnerScope&)            = delete;
    SpinnerScope& operator=(const SpinnerScope&) = delete;
    SpinnerScope(SpinnerScope&&)                 = delete;
    SpinnerScope& operator=(SpinnerScope&&)      = delete;
};

std::string sanitize_error(std::string_view msg) {
    const auto trimmed = [msg]() {
        auto first_line     = msg.substr(0, msg.find('\n'));
        auto without_prefix = trim_sv(first_line);
        if (without_prefix.starts_with("Error: ")) { without_prefix = without_prefix.substr(sizeof("Error: ") - 1); }
        return trim_sv(without_prefix);
    }();

    return trimmed | std::views::filter([](char c) { return c != '[' && c != ']'; }) | std::ranges::to<std::string>();
}

struct ParsingContext {
    SpeedEntryResult& entry;
    SpeedTestResult& result;
    bool& found_result;
};
void handle_speed_result(const SpeedTestResultJson& parsed, ParsingContext ctx) {
    if (!parsed.download || !parsed.upload || !parsed.ping) {
        ctx.entry.error = "Malformed result (missing speed data)";
        return;
    }

    ctx.entry.download_mbps = parsed.download->bandwidth * kBitsPerByte / kBitsPerMbps;
    ctx.entry.upload_mbps   = parsed.upload->bandwidth * kBitsPerByte / kBitsPerMbps;
    ctx.entry.latency_ms    = parsed.ping->latency;

    ctx.entry.loss
        = parsed.packetLoss.transform([](double loss) { return std::format("{:.2f} %", loss); }).value_or("-");

    ctx.entry.success = true;
    ctx.found_result  = true;
}

void handle_log_message(const SpeedTestResultJson& parsed, ParsingContext ctx) {
    if (parsed.level != "error") { return; }

    const auto msg = parsed.message.empty() ? "Unknown error" : parsed.message;
    if (msg.contains("Limit reached")) {
        mark_rate_limited(ctx.entry, ctx.result);
        return;
    }

    if (msg.contains("No servers defined")) {
        ctx.entry.error = "Server Offline/Changed";
        return;
    }

    if (ctx.entry.error.empty()) { ctx.entry.error = sanitize_error(msg); }
}

void process_speed_test_line(std::string_view sv, ParsingContext ctx) {
    if (sv.contains("Limit reached") || sv.contains("Too many requests")) {
        mark_rate_limited(ctx.entry, ctx.result);
        return;
    }

    SpeedTestResultJson parsed;
    if (glz::read<glz::opts { .error_on_unknown_keys = false }>(parsed, sv)) { return; }

    if (!parsed.error.empty()) {
        if (ctx.entry.error.empty()) { ctx.entry.error = sanitize_error(parsed.error); }
        return;
    }

    if (parsed.type == "result") {
        handle_speed_result(parsed, ctx);
        return;
    }

    if (parsed.type == "log") {
        handle_log_message(parsed, ctx);
        return;
    }
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

struct ParseOutputContext {
    SpeedEntryResult& entry;
    SpeedTestResult& result;
    bool& found_result;
    std::string_view& last_raw_output;
};

struct EntryFallbackContext {
    SpeedEntryResult& entry;
    const SpeedTestResult& result;
    bool found_result;
    std::string_view last_raw_output;
    std::string_view pipe_error;
    std::int32_t pipe_exit_code;
};

struct NodeRunContext {
    const std::string& cli_path;
    std::string_view cert_path;
    SpinnerCallback& spinner_cb;
    SpeedTestResult& result;
};

std::vector<std::string> build_speedtest_command_args(
    const std::string& cli_path, std::string_view cert_path, std::string_view server_id) {
    std::vector<std::string> cmd_args { cli_path, "-f", "json", "--accept-license", "--accept-gdpr",
        std::format("--ca-certificate={}", cert_path) };

    if (!server_id.empty()) { cmd_args.emplace_back(std::format("--server-id={}", server_id)); }

    return cmd_args;
}

void parse_speedtest_output_lines(std::string_view output, ParseOutputContext context) {
    for (auto line_rng : output | std::views::split('\n')) {
        std::string_view sv(line_rng);
        if (trim_sv(sv).empty()) { continue; }

        context.last_raw_output = sv;

        ParsingContext ctx { context.entry, context.result, context.found_result };
        process_speed_test_line(sv, ctx);

        if (context.found_result || context.result.rate_limited) { break; }
    }
}

void apply_entry_fallback_errors(EntryFallbackContext context) {
    if (!context.found_result && (!context.pipe_error.empty() || context.pipe_exit_code != 0)) {
        context.entry.success = false;
        if (context.entry.error.empty()) {
            context.entry.error = !context.pipe_error.empty()
                ? std::string(context.pipe_error)
                : std::format("Process failed with code {}", context.pipe_exit_code);
        }
    }

    if (!context.found_result && !context.entry.success && context.entry.error.empty()
        && !context.result.rate_limited) {
        context.entry.error = !context.last_raw_output.empty()
            ? std::format("CLI Error: {}", truncate_error(trim_sv(context.last_raw_output)))
            : "No Result Data (Empty Output)";
    }
}

NodeExecutionResult run_speed_test_for_node(const Node& node, NodeRunContext context) {
    SpinnerScope spinner(context.spinner_cb, node.name);

    auto cmd_args = build_speedtest_command_args(context.cli_path, context.cert_path, node.id);

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

    auto pipe_res = pipe->read_all(seconds(config::kSpeedtestDlTimeoutSec + 15), {}, false);
    if (pipe_res.interrupted || check_interrupted()) {
        node_result.entry.success = false;
        node_result.entry.error   = std::string { config::kInterruptMsg };
        node_result.action        = NodeExecutionAction::Break;
        return node_result;
    }

    const std::string& output = pipe_res.output;

    std::string_view last_raw_output;
    bool found_result = false;
    parse_speedtest_output_lines(output,
        ParseOutputContext {
            .entry           = node_result.entry,
            .result          = context.result,
            .found_result    = found_result,
            .last_raw_output = last_raw_output,
        });

    if (node_result.entry.rate_limited) {
        node_result.action = NodeExecutionAction::Return;
        return node_result;
    }

    apply_entry_fallback_errors(EntryFallbackContext {
        .entry           = node_result.entry,
        .result          = context.result,
        .found_result    = found_result,
        .last_raw_output = last_raw_output,
        .pipe_error      = pipe_res.error,
        .pipe_exit_code  = pipe_res.exit_code,
    });

    return node_result;
}

std::expected<std::string_view, std::string> resolve_speedtest_url_arch(std::string_view arch) {
    using Result = std::expected<std::string_view, std::monostate>;

    return Result(std::unexpected(std::monostate {}))
        .or_else([arch](auto) -> Result {
            static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kKnownArchs
                = { { { "x86_64", "x86_64" }, { "i386", "i386" }, { "i686", "i386" }, { "i586", "i386" },
                    { "aarch64", "aarch64" }, { "arm64", "aarch64" } } };
            auto it = std::ranges::find(kKnownArchs, arch, &std::pair<std::string_view, std::string_view>::first);
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
        .transform_error([&arch](auto) { return std::format("Unsupported architecture: {}", arch); });
}

std::string build_speedtest_download_url(std::string_view url_arch) {
    return std::format("https://install.speedtest.net/app/cli/ookla-speedtest-{}-linux-{}.tgz",
        config::kSpeedtestCliVersion, url_arch);
}

} // namespace

SpeedTest::SpeedTest(HttpClient& http, const std::filesystem::path& base_dir)
    : http_(http)
    , base_dir_(base_dir) {
    fs::path cli_rel(config::kSpeedtestCliPath);
    cli_dir_  = base_dir_ / cli_rel.parent_path();
    cli_path_ = base_dir_ / cli_rel;
    tgz_path_ = base_dir_ / config::kSpeedtestTgz;
}

std::expected<SpeedTest, std::string> SpeedTest::create(HttpClient& client) {
    return std::expected<std::string, std::string> { (fs::temp_directory_path() / "calyx_XXXXXX").string() }.and_then(
        [&client](std::string temp_template) -> std::expected<SpeedTest, std::string> {
            return mkdtemp(temp_template.data())
                ? std::expected<SpeedTest, std::string> { SpeedTest(client, temp_template) }
                : std::unexpected(format_sys_error(errno, "Failed to create secure temp dir"));
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

std::expected<void, std::string> SpeedTest::install() {
    std::println("Downloading Speedtest CLI...");

    return resolve_speedtest_url_arch(SystemInfo::get_raw_arch())
        .transform([](std::string_view url_arch) { return build_speedtest_download_url(url_arch); })
        .and_then([this](const std::string& url) { return http_.download(url, tgz_path_); })
        .and_then([this]() -> std::expected<void, std::string> {
            std::error_code ec;
            fs::create_directories(cli_dir_, ec);
            return ec ? std::unexpected(format_sys_error(ec.value(), "Installation Directory"))
                      : std::expected<void, std::string> {};
        })
        .and_then([this]() -> std::expected<void, std::string> {
            return archive::TgzExtractor::extract(tgz_path_, cli_dir_).transform_error([](archive::ExtractError err) {
                return std::format("Failed to extract Speedtest: {}", archive::TgzExtractor::error_string(err));
            });
        })
        .and_then([this]() -> std::expected<void, std::string> {
            return fs::exists(cli_path_) ? std::expected<void, std::string> {}
                                         : std::unexpected("Speedtest binary not found after extraction!");
        })
        .and_then([this]() -> std::expected<void, std::string> {
            std::error_code ec;
            fs::permissions(cli_path_, fs::perms::owner_all, fs::perm_options::add, ec);
            return ec ? std::unexpected(format_sys_error(ec.value(), "Permissions Update"))
                      : std::expected<void, std::string> {};
        });
}

SpeedTestResult SpeedTest::run(SpinnerCallback& spinner_cb) {
    SpeedTestResult result;
    result.entries.reserve(kServers.size());

    auto cert_expected = ScopedCertFile::create(base_dir_, std::span { cacert_pem, cacert_pem_len });

    if (!cert_expected) {
        SpeedEntryResult entry;
        entry.node_name = "System Error";
        entry.error     = std::format("Certificate Error: {}", cert_expected.error().message());
        entry.success   = false;
        result.entries.push_back(std::move(entry));
        return result;
    }

    const auto& cert           = *cert_expected;
    const std::string cli_path = cli_path_.string();

    for (const auto& node : kServers) {
        if (check_interrupted()) { break; }

        auto node_result = run_speed_test_for_node(node,
            NodeRunContext {
                .cli_path   = cli_path,
                .cert_path  = cert.get_path(),
                .spinner_cb = spinner_cb,
                .result     = result,
            });

        result.entries.push_back(std::move(node_result.entry));

        if (node_result.action == NodeExecutionAction::Return) { return result; }
        if (node_result.action == NodeExecutionAction::Break) { break; }
    }
    return result;
}

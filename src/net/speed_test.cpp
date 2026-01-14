/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/speed_test.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <sstream>
#include <ranges>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <string_view>
#include <string>
#include <vector>
#include <expected>

#include <nlohmann/json.hpp>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>

#include "include/config.hpp"
#include "include/http_client.hpp"
#include "include/interrupts.hpp"
#include "include/results.hpp"
#include "include/shell_pipe.hpp"
#include "include/utils.hpp"
#include "include/tgz_extractor.hpp"
#include "include/system_info.hpp"
#include "include/file_descriptor.hpp"

extern unsigned char cacert_pem[];
extern unsigned int cacert_pem_len;

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Node {
    std::string_view id;
    std::string_view name;
};

constexpr std::array<Node, 7> SERVERS = {{{"", "Speedtest.net (Auto)"},
                                          {"59016", "Singapore, SG"},
                                          {"5905", "Los Angeles, US"},
                                          {"59219", "Montreal, CA"},
                                          {"62493", "Paris, FR"},
                                          {"3386", "Amsterdam, NL"},
                                          {"12492", "Sydney, AU"}}};

class ScopedCertFile {
    fs::path path_;

    explicit ScopedCertFile(fs::path path) : path_(std::move(path)) {}

   public:
    ScopedCertFile(ScopedCertFile&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }

    ScopedCertFile& operator=(ScopedCertFile&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    ScopedCertFile(const ScopedCertFile&) = delete;
    ScopedCertFile& operator=(const ScopedCertFile&) = delete;

    ~ScopedCertFile() {
        cleanup();
    }

    static std::expected<ScopedCertFile, std::string> create(const fs::path& dir,
                                                             std::span<const unsigned char> data) {
        fs::path cert_path = dir / "cacert.pem";

        // Modern POSIX open with O_CLOEXEC and mode 0600 (rw-------)
        int raw_fd = ::open(cert_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (raw_fd < 0) {
            return std::unexpected(std::format("Failed to open file: {} (Code: {})",
                                               std::system_category().message(errno), errno));
        }

        // Hand over to RAII wrapper immediately
        FileDescriptor fd(raw_fd);

        const unsigned char* ptr = data.data();
        size_t remaining = data.size();

        // Robust write loop handling partial writes and EINTR
        while (remaining > 0) {
            ssize_t written = ::write(fd.get(), ptr, remaining);
            if (written < 0) {
                if (errno == EINTR) continue;
                return std::unexpected(std::format("Write failed: {} (Code: {})",
                                                   std::system_category().message(errno), errno));
            }
            ptr += written;
            remaining -= static_cast<size_t>(written);
        }

        // Ensure data hits the disk
        if (::fsync(fd.get()) < 0) {
            return std::unexpected(std::format("fsync failed: {} (Code: {})",
                                               std::system_category().message(errno), errno));
        }

        return ScopedCertFile(std::move(cert_path));
    }

    std::string get_path() const {
        return path_.string();
    }

   private:
    void cleanup() {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }
};

class SpinnerScope {
    const SpinnerCallback& cb_;
    std::string_view label_;
    bool active_ = false;

   public:
    SpinnerScope(const SpinnerCallback& cb, std::string_view label) : cb_(cb), label_(label) {
        active_ = static_cast<bool>(cb_);
        if (active_)
            cb_(SpinnerEvent::Start, label_);
    }

    ~SpinnerScope() {
        if (active_)
            cb_(SpinnerEvent::Stop, label_);
    }
};

std::string sanitize_error(std::string_view msg) {
    auto nl = msg.find('\n');
    if (nl != std::string_view::npos) {
        msg = msg.substr(0, nl);
    }
    msg = trim_sv(msg);
    if (msg.starts_with("Error: ")) {
        msg.remove_prefix(7);
    }
    return std::string(msg);
}

}  // namespace

SpeedTest::SpeedTest(HttpClient& h) : http_(h) {
    std::string temp_template = (fs::temp_directory_path() / "calyx_XXXXXX").string();
    char* path_ptr = mkdtemp(temp_template.data());

    if (!path_ptr) {
        throw std::system_error(errno, std::generic_category(), "Failed to create secure temp dir");
    }

    base_dir_ = path_ptr;

    fs::path cli_rel(Config::SPEEDTEST_CLI_PATH);
    cli_dir_ = base_dir_ / cli_rel.parent_path();
    cli_path_ = base_dir_ / cli_rel;
    tgz_path_ = base_dir_ / Config::SPEEDTEST_TGZ;
}

SpeedTest::~SpeedTest() {
    std::error_code ec;
    if (fs::exists(base_dir_, ec)) {
        fs::remove_all(base_dir_, ec);
    }
}

void SpeedTest::install() {
    std::println("Downloading Speedtest CLI...");
    std::string arch = SystemInfo::get_raw_arch();
    std::string url_arch;

    // Modern ARCH map with string_view
    static constexpr std::pair<std::string_view, std::string_view> KNOWN_ARCHS[] = {
        {"x86_64", "x86_64"},
        {"i386", "i386"},
        {"i686", "i386"},
        {"i586", "i386"},
        {"aarch64", "aarch64"},
        {"arm64", "aarch64"}};

    auto it =
        std::ranges::find_if(KNOWN_ARCHS, [&](const auto& pair) { return pair.first == arch; });

    if (it != std::end(KNOWN_ARCHS)) {
        url_arch = it->second;
    } else if (arch.starts_with("armv7")) {
        url_arch = "armhf";
    } else if (arch.starts_with("armv6") || arch.starts_with("armv5")) {
        url_arch = "armel";
    } else {
        throw std::runtime_error("Unsupported architecture: " + arch);
    }

    std::string url =
        std::format("https://install.speedtest.net/app/cli/ookla-speedtest-{}-linux-{}.tgz",
                    Config::SPEEDTEST_CLI_VERSION,
                    url_arch);

    http_.download(url, tgz_path_.string())
        .transform_error([](std::string err) { return "Download failed: " + err; })
        .and_then([this]() -> std::expected<void, std::string> {
            std::error_code ec;
            fs::create_directories(cli_dir_, ec);
            return ec ? std::unexpected("Failed to create installation directory: " + ec.message())
                      : std::expected<void, std::string>{};
        })
        .and_then([this]() -> std::expected<void, std::string> {
            return calyx::core::TgzExtractor::extract(tgz_path_, cli_dir_)
                .transform_error([](calyx::core::ExtractError err) {
                    return "Failed to extract Speedtest: " +
                           calyx::core::TgzExtractor::error_string(err);
                });
        })
        .and_then([this]() -> std::expected<void, std::string> {
            return fs::exists(cli_path_)
                       ? std::expected<void, std::string>{}
                       : std::unexpected("Speedtest binary not found after extraction!");
        })
        .and_then([this]() -> std::expected<void, std::string> {
            std::error_code ec;
            fs::permissions(cli_path_, fs::perms::owner_all, fs::perm_options::add, ec);
            return ec ? std::unexpected("Failed to set executable permissions: " + ec.message())
                      : std::expected<void, std::string>{};
        })
        .or_else([](const std::string& err) -> std::expected<void, std::string> {
            throw std::runtime_error(err);
        });
}

SpeedTestResult SpeedTest::run(const SpinnerCallback& spinner_cb) {
    SpeedTestResult result;
    result.entries.reserve(SERVERS.size());

    // Use std::span for safer access to embedded cert
    auto cert_expected = ScopedCertFile::create(base_dir_, std::span{cacert_pem, cacert_pem_len});

    if (!cert_expected) {
        SpeedEntryResult entry;
        entry.node_name = "System Error";
        entry.error = "Certificate Error: " + cert_expected.error();
        entry.success = false;
        result.entries.push_back(entry);
        return result;
    }

    const auto& cert = *cert_expected;

    for (const auto& node : SERVERS) {
        if (g_interrupted)
            break;

        SpinnerScope spinner(spinner_cb, node.name);

        std::vector<std::string> cmd_args = {
            cli_path_.string(), "-f", "json", "--accept-license", "--accept-gdpr"};

        cmd_args.push_back(std::format("--ca-certificate={}", cert.get_path()));

        if (!node.id.empty()) {
            cmd_args.push_back(std::format("--server-id={}", node.id));
        }

        SpeedEntryResult entry;
        entry.server_id = std::string(node.id);
        entry.node_name = std::string(node.name);

        try {
            ShellPipe pipe(cmd_args);
            // 90 seconds timeout
            std::string output = pipe.read_all(std::chrono::milliseconds(90000), {}, false);

            if (g_interrupted) {
                entry.success = false;
                entry.error = "Interrupted by user";
                result.entries.push_back(entry);
                break;
            }

            std::string last_raw_output;
            bool found_result = false;

            // Modern C++23: Process output line-by-line using views to avoid copies
            for (auto line_rng : std::string_view(output) | std::views::split('\n')) {
                std::string_view sv(line_rng.begin(), line_rng.end());
                if (trim_sv(sv).empty())
                    continue;

                std::string line(sv);
                last_raw_output = line;

                if (line.contains("Limit reached") || line.contains("Too many requests")) {
                    entry.rate_limited = true;
                    entry.error = "Rate Limit Reached";
                    result.rate_limited = true;
                    break;
                }

                try {
                    auto j = json::parse(line);

                    if (j.contains("error")) {
                        if (j.at("error").is_string()) {
                            entry.error = sanitize_error(j.at("error").get<std::string>());
                        } else {
                            entry.error = "Unknown CLI Error";
                        }
                        continue;
                    }

                    std::string type = j.value("type", "");

                    if (type == "result") {
                        if (!j.contains("download") || !j.contains("upload")) {
                            entry.error = "Malformed result (missing speed data)";
                            continue;
                        }

                        double dl_bytes = j.at("download").value("bandwidth", 0.0);
                        double ul_bytes = j.at("upload").value("bandwidth", 0.0);

                        entry.download_mbps = (dl_bytes * 8.0) / 1'000'000.0;  // Digit separators
                        entry.upload_mbps = (ul_bytes * 8.0) / 1'000'000.0;

                        if (j.contains("ping")) {
                            entry.latency_ms = j.at("ping").value("latency", 0.0);
                        } else {
                            entry.latency_ms = 0.0;
                        }

                        if (j.contains("packetLoss")) {
                            double loss = j.value("packetLoss", 0.0);
                            entry.loss = std::format("{:.2f} %", loss);
                        } else {
                            entry.loss = "-";
                        }

                        entry.success = true;
                        found_result = true;
                        break;
                    } else if (type == "log") {
                        std::string level = j.value("level", "");
                        if (level == "error") {
                            std::string msg = j.value("message", "Unknown error");
                            if (msg.contains("Limit reached")) {  // Modern contains
                                entry.rate_limited = true;
                                entry.error = "Rate Limit Reached";
                                result.rate_limited = true;
                            } else if (msg.contains("No servers defined")) {
                                entry.error = "Server Offline/Changed";
                            } else {
                                entry.error = sanitize_error(msg);
                            }
                        }
                    }

                } catch (const json::parse_error&) {
                    continue;  // Skip non-JSON lines
                }
            }

            if (!found_result && !entry.success && entry.error.empty() && !result.rate_limited) {
                if (!last_raw_output.empty()) {
                    std::string clean_msg = trim(last_raw_output);
                    if (clean_msg.length() > 50)
                        clean_msg = clean_msg.substr(0, 47) + "...";
                    entry.error = "CLI Error: " + clean_msg;
                } else {
                    entry.error = "No Result Data (Empty Output)";
                }
            }

            if (entry.rate_limited) {
                result.entries.push_back(entry);
                return result;
            }

        } catch (const std::exception& e) {
            entry.error = e.what();
            entry.success = false;
        }
        result.entries.push_back(entry);
    }

    return result;
}

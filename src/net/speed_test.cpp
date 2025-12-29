#include "include/speed_test.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/utsname.h>
#include <nlohmann/json.hpp>

#include "include/config.hpp"
#include "include/http_client.hpp"
#include "include/interrupts.hpp"
#include "include/results.hpp"
#include "include/shell_pipe.hpp"
#include "include/utils.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

class SpinnerScope {
    const SpinnerCallback& cb_;
    std::string_view label_;
    bool active_ = false;

public:
    SpinnerScope(const SpinnerCallback& cb, std::string_view label) : cb_(cb), label_(label) {
        active_ = static_cast<bool>(cb_);
        if (active_) cb_(SpinnerEvent::Start, label_);
    }

    ~SpinnerScope() {
        if (active_) cb_(SpinnerEvent::Stop, label_);
    }
};

std::string sanitize_error(std::string_view msg) {
    auto nl = msg.find('\n');
    if (nl != std::string_view::npos) {
        msg = msg.substr(0, nl);
    }

    while (!msg.empty() && std::isspace(static_cast<unsigned char>(msg.back()))) {
        msg.remove_suffix(1);
    }

    while (!msg.empty() && std::isspace(static_cast<unsigned char>(msg.front()))) {
        msg.remove_prefix(1);
    }
    
    if (msg.starts_with("Error: ")) {
        msg.remove_prefix(7);
    }
    
    return std::string(msg);
}

}

SpeedTest::SpeedTest(HttpClient& h) : http_(h) {
    std::string temp_template = (fs::temp_directory_path() / "bench_XXXXXX").string();
    
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

std::string SpeedTest::get_arch() {
    struct utsname buf; uname(&buf);
    std::string m(buf.machine);
    if (m == "x86_64") return "x86_64";
    if (m == "aarch64" || m == "arm64") return "aarch64";
    if (m == "i386" || m == "i686") return "i386";
    if (m == "armv7l") return "armhf";
    throw std::runtime_error("Unsupported architecture: " + m);
}

void SpeedTest::install() {
    std::println("Downloading Speedtest CLI...");
    std::string url = std::format("https://install.speedtest.net/app/cli/ookla-speedtest-1.2.0-linux-{}.tgz", get_arch());

    auto result = http_.download(url, tgz_path_.string());
    if (!result) {
        throw std::runtime_error(result.error());
    }

    fs::create_directories(cli_dir_);

    std::vector<std::string> tar_args = {
        "tar", "zxf", tgz_path_.string(), "-C", cli_dir_.string()
    };
    
    ShellPipe pipe(tar_args);
    pipe.read_all();

    if (!fs::exists(cli_path_)) throw std::runtime_error("Failed to extract speedtest-cli");
    fs::permissions(cli_path_, fs::perms::owner_all, fs::perm_options::add);
}

SpeedTestResult SpeedTest::run(const SpinnerCallback& spinner_cb) {
    struct Node { std::string id; std::string name; };

    std::vector<Node> nodes = {
        {"", "Speedtest.net (Auto)"},
        {"59016", "Singapore, SG"},
        {"5905", "Los Angeles, US"},
        {"59219",  "Montreal, CA"},
        {"41840",  "Paris, FR"},
        {"3386",  "Amsterdam, NL"},
        {"46114", "Melbourne, AU"}
    };

    SpeedTestResult result;

    for (const auto& node : nodes) {
        if (g_interrupted) break;

        SpinnerScope spinner(spinner_cb, node.name);

        std::vector<std::string> cmd_args = {
            cli_path_.string(),
            "-f", "json",
            "--accept-license", "--accept-gdpr"
        };

        if (!node.id.empty()) {
            cmd_args.push_back("--server-id=" + node.id);
        }

        SpeedEntryResult entry;
        entry.server_id = node.id;
        entry.node_name = node.name;

        try {
            ShellPipe pipe(cmd_args);
            std::string output = pipe.read_all();
            
            if (g_interrupted) {
                entry.success = false;
                entry.error = "Interrupted by user";
                result.entries.push_back(entry);
                break;
            }

            std::stringstream ss(output);
            std::string line;
            std::string last_raw_output;
            bool found_result = false;

            while(std::getline(ss, line)) {
                if (trim(line).empty()) continue;
                last_raw_output = line;

                if (line.find("Limit reached") != std::string::npos || 
                    line.find("Too many requests") != std::string::npos) {
                    entry.rate_limited = true;
                    entry.error = "Rate Limit Reached";
                    result.rate_limited = true;
                    break;
                }

                try {
                    auto j = json::parse(line);
                    
                    if (j.contains("error")) {
                        if (j["error"].is_string()) {
                            entry.error = sanitize_error(j["error"].get<std::string>());
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

                        double dl_bytes = j["download"].value("bandwidth", 0.0);
                        double ul_bytes = j["upload"].value("bandwidth", 0.0);
                        
                        entry.download_mbps = (dl_bytes * 8.0) / 1000000.0;
                        entry.upload_mbps = (ul_bytes * 8.0) / 1000000.0;
                        
                        if (j.contains("ping")) {
                            entry.latency_ms = j["ping"].value("latency", 0.0);
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
                    }
                    else if (type == "log") {
                        std::string level = j.value("level", "");
                        if (level == "error") {
                            std::string msg = j.value("message", "Unknown error");
                            if (msg.find("Limit reached") != std::string::npos) {
                                entry.rate_limited = true;
                                entry.error = "Rate Limit Reached";
                                result.rate_limited = true;
                            } else if (msg.find("No servers defined") != std::string::npos) {
                                entry.error = "Server Offline/Changed";
                            } else {
                                entry.error = sanitize_error(msg);
                            }
                        }
                    }

                } catch (const json::parse_error&) {
                    continue;
                }
            }

            if (!found_result && !entry.success && entry.error.empty()) {
                if (result.rate_limited) {
                     
                } else if (!last_raw_output.empty()) {
                     std::string clean_msg = trim(last_raw_output);
                     if (clean_msg.length() > 50) clean_msg = clean_msg.substr(0, 47) + "...";
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
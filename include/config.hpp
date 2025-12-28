#pragma once

#include <cstddef>
#include <string_view>

namespace Config {
    constexpr std::size_t IO_BLOCK_SIZE = 4 * 1024 * 1024;
    constexpr std::size_t IO_ALIGNMENT = 4096;
    constexpr std::string_view BENCH_FILENAME = "benchtest_file";
    constexpr std::string_view SPEEDTEST_CLI_PATH = "speedtest-cli/speedtest";
    constexpr std::string_view SPEEDTEST_TGZ = "speedtest.tgz";
    
    constexpr long HTTP_TIMEOUT_SEC = 10;
    constexpr long HTTP_CONNECT_TIMEOUT_SEC = 10;
    constexpr long SPEEDTEST_DL_TIMEOUT_SEC = 60;
    constexpr int UI_SPINNER_DELAY_MS = 150;
}
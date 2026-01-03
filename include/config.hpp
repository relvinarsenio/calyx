#pragma once

#include <cstddef>
#include <string_view>

namespace Config {
    constexpr int DISK_IO_RUNS = 3;
    constexpr int DISK_TEST_SIZE_MB = 1024;
    constexpr int IO_LABEL_WIDTH = 22;
    constexpr int PROGRESS_BAR_WIDTH = 26;

    constexpr int IO_WRITE_QUEUE_DEPTH = 16;
    constexpr int IO_READ_QUEUE_DEPTH = 16;
    constexpr std::size_t IO_WRITE_BLOCK_SIZE = 1 * 1024 * 1024;
    constexpr std::size_t IO_READ_BLOCK_SIZE = 1 * 1024 * 1024;
    constexpr std::size_t IO_ALIGNMENT = 4096;
    constexpr std::string_view BENCH_FILENAME = "benchtest_file";

    constexpr bool IO_URING_ENABLED = true;
    constexpr std::size_t PIPE_MAX_OUTPUT_BYTES = 10 * 1024 * 1024;
    constexpr std::string_view SPEEDTEST_CLI_PATH = "speedtest-cli/speedtest";
    constexpr std::string_view SPEEDTEST_TGZ = "speedtest.tgz";
    constexpr std::string_view HTTP_USER_AGENT =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/120.0.0.0 Safari/537.36";
    
    constexpr long HTTP_TIMEOUT_SEC = 10;
    constexpr long HTTP_CONNECT_TIMEOUT_SEC = 10;
    constexpr long SPEEDTEST_DL_TIMEOUT_SEC = 60;

    constexpr long DISK_BENCH_MAX_SECONDS = 600;

    constexpr long CHECK_CONN_TIMEOUT_SEC = 5;
    constexpr long CHECK_CONN_CONNECT_TIMEOUT_SEC = 3;

    constexpr int UI_SPINNER_DELAY_MS = 150;
}
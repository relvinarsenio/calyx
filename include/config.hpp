/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
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
constexpr std::size_t TERM_WIDTH = 80;
constexpr std::string_view TEST_FILENAME = "calyx_test_file";

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

constexpr long DISK_BENCHMARK_MAX_SECONDS = 600;

constexpr long CHECK_CONN_TIMEOUT_SEC = 5;
constexpr long CHECK_CONN_CONNECT_TIMEOUT_SEC = 3;

constexpr int UI_SPINNER_DELAY_MS = 150;
constexpr bool UI_FORCE_ASCII = false;

// TGZ Extractor Security Limits
constexpr std::uint64_t TGZ_MAX_FILE_SIZE = 100 * 1024 * 1024;   // 100MB per file
constexpr std::uint64_t TGZ_MAX_TOTAL_SIZE = 500 * 1024 * 1024;  // 500MB total
constexpr std::uint32_t TGZ_MAX_FILES = 10000;                   // Max files in archive
constexpr std::uint32_t TGZ_MAX_PATH_DEPTH = 20;                 // Max directory depth
constexpr std::uint32_t TGZ_MAX_PATH_LENGTH = 255;               // Max single component length
constexpr std::uint32_t TGZ_MAX_TOTAL_PATH_LENGTH = 4096;        // Max total path length

// TAR Format Constants
constexpr std::size_t TAR_BLOCK_SIZE = 512;
constexpr std::size_t TAR_NAME_OFFSET = 0;
constexpr std::size_t TAR_NAME_LENGTH = 100;
constexpr std::size_t TAR_MODE_OFFSET = 100;
constexpr std::size_t TAR_MODE_LENGTH = 8;
constexpr std::size_t TAR_SIZE_OFFSET = 124;
constexpr std::size_t TAR_SIZE_LENGTH = 12;
constexpr std::size_t TAR_CHECKSUM_OFFSET = 148;
constexpr std::size_t TAR_CHECKSUM_LENGTH = 8;
constexpr std::size_t TAR_TYPE_OFFSET = 156;
constexpr std::size_t TAR_PREFIX_OFFSET = 345;
constexpr std::size_t TAR_PREFIX_LENGTH = 155;

// Application Display Constants
constexpr std::string_view APP_NAME = "calyx";
constexpr std::string_view APP_VERSION = "7.2.1";

// Display Constants
constexpr int APP_AUTHOR_LABEL_WIDTH = 18;
constexpr int APP_INFO_LABEL_WIDTH = 20;
constexpr int APP_SWAP_LABEL_WIDTH = 22;
constexpr double TIME_MINUTES_THRESHOLD = 60.0;
constexpr double SECONDS_PER_MINUTE = 60.0;
}  // namespace Config
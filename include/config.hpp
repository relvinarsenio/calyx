/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "numeric_cast.hpp"

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

/**
 * @namespace config
 * @brief Global configuration constants and build-time settings.
 */
namespace config {
/** @brief Auto-detect CPU core for thread affinity pinning (-1). */
inline constexpr std::int32_t kAffinityAutoCore = -1;
/** @brief Number of iterations for disk I/O benchmarks. */
inline constexpr std::uint8_t kDiskIoRuns = 3;
/** @brief Size of the temporary file used for disk tests (in megabytes). */
inline constexpr std::uint64_t kDiskTestSizeMb = 1024;
/** @brief Display width for I/O labels in the UI. */
inline constexpr std::uint8_t kIoLabelWidth = 22;
/** @brief Column width for progress bars. */
inline constexpr std::uint8_t kProgressBarWidth = 26;

/** @brief Depth of the io_uring write queue. */
inline constexpr std::uint16_t kIoWriteQueueDepth = 16;
/** @brief Depth of the io_uring read queue. */
inline constexpr std::uint16_t kIoReadQueueDepth = 16;
/** @brief Percentage of the queue depth to wait for during io_uring batching (e.g., 25 means 25%). */
inline constexpr std::uint32_t kIoBatchPercent = 25;
/** @brief Block size for write operations (1 MiB). */
inline constexpr std::size_t kIoWriteBlockSize = 1z * 1024z * 1024z;
/** @brief Block size for read operations (1 MiB). */
inline constexpr std::size_t kIoReadBlockSize = 1z * 1024z * 1024z;
/** @brief Alignment requirement for direct I/O buffers (4 KiB page size). */
inline constexpr std::size_t kIoAlignment = 4096z;
/** @brief Baseline terminal width for formatting. */
inline constexpr std::size_t kTermWidth = 100z;

/**
 * @brief Rounds @p requested size up to the nearest multiple of @c kIoAlignment.
 */
constexpr std::size_t get_buffer_size(std::size_t requested) {
    static_assert(std::has_single_bit(kIoAlignment), "kIoAlignment must be a power of two");
    return requested == 0z
        ? 0z
        : ((requested & (kIoAlignment - 1)) == 0z ? requested : ((requested / kIoAlignment) + 1z) * kIoAlignment);
}

/**
 * @brief Compile-time validation (must be after declarations).
 */
static_assert(kDiskIoRuns > 0, "kDiskIoRuns must be positive");
static_assert(kDiskTestSizeMb > 0, "kDiskTestSizeMb must be positive");
static_assert(kIoWriteQueueDepth > 0, "kIoWriteQueueDepth must be positive");
static_assert(kIoReadQueueDepth > 0, "kIoReadQueueDepth must be positive");
static_assert(kIoWriteBlockSize > 0, "kIoWriteBlockSize must be positive");
static_assert(kIoReadBlockSize > 0, "kIoReadBlockSize must be positive");
static_assert((kIoWriteBlockSize & (kIoAlignment - 1)) == 0z, "kIoWriteBlockSize alignment");
static_assert((kIoReadBlockSize & (kIoAlignment - 1)) == 0z, "kIoReadBlockSize alignment");

/**
 * @brief UI table column widths.
 */
inline constexpr std::uint8_t kUiTableNodeWidth    = 24;
inline constexpr std::uint8_t kUiTableDlWidth      = 18;
inline constexpr std::uint8_t kUiTableUlWidth      = 18;
inline constexpr std::uint8_t kUiTableLatencyWidth = 12;
inline constexpr std::uint8_t kUiTableLossWidth    = 8;
inline constexpr std::size_t kMaxErrorDisplayLen   = (kTermWidth > toSize(kUiTableNodeWidth) + (sizeof("Error: ") - 1))
      ? kTermWidth - toSize(kUiTableNodeWidth) - (sizeof("Error: ") - 1)
      : 0;

/** @brief Basename for the temporary disk benchmark file. */
inline constexpr std::string_view kTestFilename = "calyx_test_file";
/** @brief User-facing message when an operation is cancelled via signal. */
inline constexpr std::string_view kInterruptMsg = "Interrupted by user";

inline constexpr std::size_t kPipeMaxOutputBytes    = 10z * 1024z * 1024z;
inline constexpr std::string_view kSpeedtestCliPath = "speedtest-cli/speedtest";
inline constexpr std::string_view kSpeedtestTgz     = "speedtest.tgz";

inline constexpr auto kHttpTimeout { std::chrono::seconds { 10 } };
inline constexpr auto kHttpConnectTimeout { std::chrono::seconds { 10 } };
inline constexpr auto kSpeedtestDlTimeout { std::chrono::seconds { 60 } };

inline constexpr auto kDiskBenchmarkMaxDuration { std::chrono::seconds { 360 } };

/** @brief Connection check overall timeout. */
inline constexpr auto kCheckConnTimeout { std::chrono::seconds { 5 } };
/** @brief Connection check handshaking timeout. */
inline constexpr auto kCheckConnConnectTimeout { std::chrono::seconds { 3 } };

inline constexpr auto kUiSpinnerDelay { std::chrono::milliseconds { 150 } };
inline constexpr auto kUiUpdateInterval { std::chrono::milliseconds { 33 } };
inline constexpr bool kUiForceAscii = false;

/**
 * @brief Speed display formatting thresholds.
 */
inline constexpr double kMbpsToGbpsThreshold = 1000.0;

inline constexpr std::uint8_t kTcpTtl = 128;
inline constexpr auto kShellPipeTermWait { std::chrono::milliseconds { 256 } };
inline constexpr auto kShellPipePollInterval { std::chrono::milliseconds { 16 } };
inline constexpr auto kShellPipeKillWait { std::chrono::seconds { 2 } };
inline constexpr auto kShellPipeDefaultTimeout { std::chrono::milliseconds { 60000 } };
inline constexpr std::size_t kFileReadChunkSize = 4096z;
inline constexpr std::size_t kPipeBufferSize    = 4096z;
inline constexpr auto kUringWaitTimeout { std::chrono::milliseconds { 200 } };

inline constexpr std::uint64_t kMinBufferBytes           = 1048576;
inline constexpr std::uint64_t kTgzMaxFileSize           = 100ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kTgzMaxTotalSize          = 500ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kTgzMaxFiles              = 10000;
inline constexpr std::uint32_t kTgzMaxPathDepth          = 20;
inline constexpr std::uint32_t kTgzMaxPathLength         = 255;
inline constexpr std::uint32_t kTgzMaxTotalPathLength    = 4096;
inline constexpr std::size_t kTgzDecompressionBufferSize = 16z * 1024z;

inline constexpr std::size_t kTarBlockSize      = 512z;
inline constexpr std::size_t kTarNameOffset     = 0z;
inline constexpr std::size_t kTarNameLength     = 100z;
inline constexpr std::size_t kTarModeOffset     = 100z;
inline constexpr std::size_t kTarModeLength     = 8z;
inline constexpr std::size_t kTarSizeOffset     = 124z;
inline constexpr std::size_t kTarSizeLength     = 12z;
inline constexpr std::size_t kTarChecksumOffset = 148z;
inline constexpr std::size_t kTarChecksumLength = 8z;
inline constexpr std::size_t kTarTypeOffset     = 156z;
inline constexpr std::size_t kTarPrefixOffset   = 345z;
inline constexpr std::size_t kTarPrefixLength   = 155z;

/** @brief Application metadata. */
inline constexpr std::string_view kAppName             = "calyx";
inline constexpr std::string_view kAppVersion          = "1.1.0";
inline constexpr std::string_view kSpeedtestCliVersion = "1.2.0";

/** @brief Network URLs and targets. */
inline constexpr std::string_view kUrlCloudflareMeta = "https://speed.cloudflare.com/meta";
inline constexpr std::string_view kUrlMaintainer     = "https://calyx.pages.dev/";
inline constexpr std::string_view kUrlGithub         = "https://github.com/relvinarsenio/calyx";
inline constexpr std::string_view kPingTargetIPv4    = "1.1.1.1";
inline constexpr std::string_view kPingTargetIPv6    = "[2606:4700:4700::1111]";

inline constexpr std::uint8_t kAppAuthorLabelWidth = 18;
inline constexpr std::uint8_t kAppInfoLabelWidth   = 20;
inline constexpr std::uint8_t kAppSwapLabelWidth   = 22;
} // namespace config
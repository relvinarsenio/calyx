/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "affinity.hpp"
#include "config.hpp"
#include "posix_error.hpp"
#include "results.hpp"
#include "uring_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

struct BenchmarkError {
    enum class Phase {
        Configuration,
        SpaceCheck,
        BufferAllocation,
        EngineSetup,
        WritePhase,
        ReadPhase
    } phase;

    std::variant<uring::UringError, affinity::IsolationError, std::error_code, posix::SysCallError> cause;
};

class DiskBenchmark {
public:
    struct BenchmarkConfig {
        std::uint64_t size_mb           = config::kDiskTestSizeMb;
        std::size_t write_block_size    = config::kIoWriteBlockSize;
        std::size_t read_block_size     = config::kIoReadBlockSize;
        std::uint16_t write_queue_depth = config::kIoWriteQueueDepth;
        std::uint16_t read_queue_depth  = config::kIoReadQueueDepth;
        std::size_t alignment           = config::kIoAlignment;
        std::string label               = "Disk Speed";
    };

    [[nodiscard]] static std::string format_error(const BenchmarkError& err);

    [[nodiscard]] static std::expected<DiskIORunResult, BenchmarkError> run_io_test(const BenchmarkConfig& config,
        const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const>& progress_cb = {},
        std::stop_token stop = {}, const std::move_only_function<bool() const noexcept>& interrupt_cb = {});
};
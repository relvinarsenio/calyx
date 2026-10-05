/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "config.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

/**
 * @brief Execution status of a ShellPipe command.
 */
enum class ShellPipeStatus {
    /** @brief The process executed successfully and exited with code 0. */
    success,
    /** @brief The process completed but exited with a non-zero exit code. */
    nonzero_exit,
    /** @brief The process or I/O operation timed out. */
    timed_out,
    /** @brief The operation was interrupted (e.g. via stop token or SIGINT). */
    interrupted,
    /** @brief The process was terminated by a signal. */
    signaled,
    /** @brief An execution, I/O, or system error occurred. */
    error
};

/**
 * @brief Result of a ShellPipe execution.
 */
struct ShellPipeResult {
    /** @brief The execution status. */
    ShellPipeStatus status = ShellPipeStatus::success;
    /** @brief The standard output and standard error of the command. */
    std::string output {};
    /** @brief The error message, if any occurred. */
    std::string error {};
    /** @brief The process exit code (typically 0 on success). */
    std::int32_t exit_code = 0;
    /** @brief The signal that terminated the process, if any. */
    std::optional<std::int32_t> signal;
    /** @brief Whether the output was truncated. */
    bool truncated = false;

    /** @brief Check if the command executed successfully. */
    [[nodiscard]] bool ok() const noexcept { return status == ShellPipeStatus::success; }
};

/**
 * @brief Facilitates execution of external processes and captures their output.
 *
 * Construction and process lifetime details are kept private. Callers only
 * provide command arguments and consume the resulting execution report.
 */
class ShellPipe {
    class Impl;
    std::unique_ptr<Impl> impl_;

    explicit ShellPipe(std::unique_ptr<Impl> impl) noexcept;

public:
    /**
     * @brief Creates a ShellPipe for the given command.
     *
     * Returns an error_code on failure instead of throwing.
     *
     * @param args Command and arguments (args[0] = executable name).
     * @return     Ready-to-read ShellPipe, or the captured error.
     */
    [[nodiscard]] static auto create(std::vector<std::string> args) -> std::expected<ShellPipe, std::error_code>;

    ~ShellPipe() noexcept;

    ShellPipe(const ShellPipe&)            = delete;
    ShellPipe& operator=(const ShellPipe&) = delete;

    ShellPipe(ShellPipe&&) noexcept;
    ShellPipe& operator=(ShellPipe&&) noexcept;

    /**
     * @brief Executes the command and returns its captured output and termination result.
     *
     * @param timeout         Maximum duration to wait for child output and termination.
     * @param stop            Stop token for cooperative cancellation.
     * @param raise_on_error  Whether non-zero exit codes should be treated as errors.
     * @return                The execution result.
     */
    [[nodiscard]] ShellPipeResult read_all(std::chrono::milliseconds timeout = config::kShellPipeDefaultTimeout,
        std::stop_token stop = {}, std::move_only_function<bool() const noexcept> interrupt_cb = {},
        bool raise_on_error = false);
};
/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "config.hpp"
#include "file_descriptor.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

/**
 * @brief Result of a ShellPipe execution.
 */
struct ShellPipeResult {
    /** @brief The standard output and standard error of the command. */
    std::string output;
    /** @brief The process exit code (typically 0 on success). */
    std::int32_t exit_code = 0;
    /** @brief The error message, if any occurred. */
    std::string error;
    /** @brief Whether the execution was interrupted. */
    bool interrupted = false;

    /** @brief Check if the command executed successfully without errors or interruption. */
    [[nodiscard]] bool ok() const noexcept { return exit_code == 0 && error.empty() && !interrupted; }
};

/**
 * @brief Facilitates execution of external processes and captures their output.
 *
 * Constructed via the static @ref create() factory method, which returns
 * @c std::expected to avoid exceptions for predictable failures like
 * missing executables.
 */
namespace sp_impl {

/**
 * @brief RAII wrapper for a child process ID.
 *
 * Automatically terminates and reaps the child process on destruction.
 * Move-only, non-copyable.
 */
class child_process {
    std::int32_t pid_ = -1;

    void terminate() noexcept;

public:
    child_process() noexcept = default;
    explicit child_process(std::int32_t pid) noexcept
        : pid_(pid) {}
    ~child_process() noexcept { reset(); }

    child_process(const child_process&)            = delete;
    child_process& operator=(const child_process&) = delete;

    child_process(child_process&& other) noexcept
        : pid_(std::exchange(other.pid_, -1)) {}

    child_process& operator=(child_process&& other) noexcept {
        if (this != &other) {
            reset();
            pid_ = std::exchange(other.pid_, -1);
        }
        return *this;
    }

    [[nodiscard]] auto native_handle() const noexcept -> std::int32_t { return pid_; }
    auto release() noexcept -> std::int32_t { return std::exchange(pid_, -1); }
    void reset(std::int32_t new_pid = -1) noexcept;

    explicit operator bool() const noexcept {
        return static_cast<bool>(posix::expect_result<posix::error_style::posix>(pid_));
    }
};

} // namespace sp_impl

class ShellPipe {
    posix::file_descriptor read_fd_;
    sp_impl::child_process pid_;

    /** @brief Private constructor — use @ref create() instead. */
    ShellPipe() = default;

public:
    /**
     * @brief Factory method: creates a ShellPipe for the given command.
     *
     * Resolves the executable, creates a pipe, forks, and execs.
     * Returns an error_code on failure instead of throwing.
     *
     * @param args Command and arguments (args[0] = executable name).
     * @return     Ready-to-read ShellPipe, or the captured error.
     */
    [[nodiscard]] static auto create(std::vector<std::string> args) -> std::expected<ShellPipe, std::error_code>;

    ~ShellPipe() noexcept = default;

    ShellPipe(const ShellPipe&)            = delete;
    ShellPipe& operator=(const ShellPipe&) = delete;

    ShellPipe(ShellPipe&& other) noexcept            = default;
    ShellPipe& operator=(ShellPipe&& other) noexcept = default;

    /**
     * @brief Reads all output from the child process and waits for its termination.
     *
     * @param timeout         Maximum duration to wait for child output and termination.
     * @param stop            Stop token for cooperative cancellation.
     * @param raise_on_error  Whether non-zero exit codes should be treated as errors.
     * @return                The execution result.
     */
    [[nodiscard]] ShellPipeResult read_all(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(config::kShellPipeDefaultTimeoutMs),
        std::stop_token stop = {}, bool raise_on_error = false);
};
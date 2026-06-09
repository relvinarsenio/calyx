/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

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
    /** @brief The standard output of the command. */
    std::string output;
    /** @brief The process exit code (typically 0 on success). */
    std::int32_t exit_code = 0;
    /** @brief The standard error output of the command. */
    std::string error;
    /** @brief Whether the operation was interrupted by the user or a signal. */
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
class ShellPipe {
    posix::file_descriptor read_fd_;
    std::int32_t pid_ = -1;
    void terminate_child() noexcept;

    /// Private constructor — use @ref create() instead.
    ShellPipe() = default;

public:
    /**
     * @brief Factory method: creates a ShellPipe for the given command.
     *
     * Resolves the executable, creates a pipe, forks, and execs.
     * Returns an error_code on failure instead of throwing, so callers
     * can gracefully continue (e.g., the speed benchmark continues
     * even if 'ookla' is not installed).
     *
     * @param args Command and arguments (args[0] = executable name).
     * @return     Ready-to-read ShellPipe, or the captured error.
     */
    [[nodiscard]] static auto create(std::vector<std::string> args) -> std::expected<ShellPipe, std::error_code>;

    ~ShellPipe() noexcept;

    ShellPipe(const ShellPipe&)            = delete;
    ShellPipe& operator=(const ShellPipe&) = delete;

    ShellPipe(ShellPipe&& other) noexcept;
    ShellPipe& operator=(ShellPipe&& other) noexcept;

    ShellPipeResult read_all(std::chrono::milliseconds timeout = std::chrono::milliseconds(60000),
        std::stop_token stop = {}, bool raise_on_error = false);
};
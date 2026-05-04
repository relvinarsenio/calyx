/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "shell_pipe.hpp"

#include "config.hpp"
#include "file_descriptor.hpp"
#include "interrupts.hpp"
#include "posix.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <expected>
#include <format>
#include <ranges>
#include <thread>
#include <vector>

namespace {} // namespace

auto ShellPipe::create(std::vector<std::string> args) -> std::expected<ShellPipe, std::error_code> {
    if (args.empty()) [[unlikely]] { return std::unexpected(std::make_error_code(std::errc::invalid_argument)); }

    auto resolved_exec = posix::resolve_executable(args[0]);
    if (!resolved_exec) { return std::unexpected(resolved_exec.error()); }

    /**
     * @brief Prepare the argument vector for exec.
     *
     * We capture pointers to the string data in `args`. This is safe because
     * `args` is a local copy that lives for the duration of this function,
     * and after fork(), the child has its own copy of the memory.
     */
    auto argv
        = args | std::views::transform([](std::string& s) { return s.data(); }) | std::ranges::to<std::vector<char*>>();
    argv.push_back(nullptr);

    auto pipe_result = posix::pipe::create();
    if (!pipe_result) { return std::unexpected(pipe_result.error()); }

    ShellPipe self;
    self.read_fd_ = pipe_result->release_read();
    auto write_fd = pipe_result->release_write();

    auto fork_result = posix::fork();
    if (!fork_result) { return std::unexpected(fork_result.error()); }

    if (*fork_result == 0) {
        /// Post-fork child: only async-signal-safe calls allowed.
        if (!write_fd.redirect_to(posix::file_descriptor::stdout_fd, false)) { ::_exit(errno); }
        if (!write_fd.redirect_to(posix::file_descriptor::stderr_fd, false)) { ::_exit(errno); }

        self.read_fd_.reset();
        write_fd.reset();

        posix::exec_fd(resolved_exec->fd.native_handle(), resolved_exec->path.c_str(), argv.data());

        /// exec only returns on failure — write error msg and exit.
        std::string_view msg = "Failed to execute binary\n";
        [[maybe_unused]] auto _
            = posix::file_descriptor::write_raw(posix::file_descriptor::stdout_fd, std::as_bytes(std::span { msg }));

        ::_exit(127);
    }

    self.pid_ = *fork_result;
    return self;
}

ShellPipe::ShellPipe(ShellPipe&& other) noexcept
    : read_fd_(std::move(other.read_fd_))
    , pid_(std::exchange(other.pid_, -1)) {}

ShellPipe& ShellPipe::operator=(ShellPipe&& other) noexcept {
    if (this != &other) {
        terminate_child();
        read_fd_ = std::move(other.read_fd_);
        pid_     = std::exchange(other.pid_, -1);
    }
    return *this;
}

void ShellPipe::terminate_child() noexcept {
    if (pid_ <= 0) [[unlikely]] { return; }

    auto wait_reap = [this](std::chrono::milliseconds timeout) noexcept -> bool {
        try {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            while (true) {
                auto wait_res = posix::waitpid(pid_, WNOHANG);
                if (wait_res && wait_res->has_value()) { return true; }
                if (!wait_res && wait_res.error() == std::errc::no_child_process) { return true; }
                if (!wait_res) { break; }
                if (std::chrono::steady_clock::now() >= deadline) { break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(config::kShellPipePollIntervalMs));
            }
        } catch (...) {}
        return false;
    };

    auto term_res = posix::kill(pid_, posix::signal::Term);
    if (!term_res && term_res.error() != std::errc::no_such_process) {
#ifdef __cpp_exceptions
        try {
#endif
            print_warning(format_sys_error(term_res.error().value(), "kill (SIGTERM) failed"));
#ifdef __cpp_exceptions
        } catch (...) {}
#endif
    }

    if ((!term_res && term_res.error() == std::errc::no_such_process)
        || wait_reap(std::chrono::milliseconds(config::kShellPipeTermWaitMs))) {
        pid_ = -1;
        return;
    }

    auto kill_res = posix::kill(pid_, posix::signal::Kill);
    if (!kill_res && kill_res.error() != std::errc::no_such_process) {
#ifdef __cpp_exceptions
        try {
#endif
            print_warning(format_sys_error(kill_res.error().value(), "kill (SIGKILL) failed"));
#ifdef __cpp_exceptions
        } catch (...) {}
#endif
    }

    if ((!kill_res && kill_res.error() == std::errc::no_such_process)
        || wait_reap(std::chrono::seconds(config::kShellPipeKillWaitSec))) {
        pid_ = -1;
        return;
    }
}

ShellPipe::~ShellPipe() noexcept {
    read_fd_.reset();

    terminate_child();
}

ShellPipeResult ShellPipe::read_all(std::chrono::milliseconds timeout, std::stop_token stop, bool raise_on_error) {
    ShellPipeResult result;
    const auto is_stopped = [&stop]() noexcept { return check_interrupted() || stop.stop_requested(); };

    const auto read_error = [this, timeout, &is_stopped, &result]() -> std::optional<std::string> {
        const auto deadline               = std::chrono::steady_clock::now() + timeout;
        const std::size_t max_output_size = config::kPipeMaxOutputBytes;
        std::array<char, config::kPipeBufferSize> buffer {};
        std::size_t total_read = 0;

        while (true) {
            if (is_stopped()) { return std::string { config::kInterruptMsg }; }

            const auto now      = std::chrono::steady_clock::now();
            const auto ms_count = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (ms_count <= 0) {
                terminate_child();
                return "Child process timed out while reading output";
            }

            pollfd pfd { .fd = read_fd_.native_handle(), .events = POLLIN, .revents = 0 };
            if (pfd.fd < 0) { return "Internal Error: Invalid file descriptor in read loop"; }

            const auto poll_res = posix::poll(std::span { &pfd, 1 }, std::chrono::milliseconds(ms_count));
            if (!poll_res) {
                terminate_child();
                return format_sys_error(poll_res.error().value(), "poll failed on child output");
            }
            if (*poll_res == 0) { continue; }

            const auto read_result = read_fd_.read(std::as_writable_bytes(std::span { buffer }), is_stopped);
            if (!read_result) {
                if (read_result.error() == std::errc::operation_canceled) {
                    return std::string { config::kInterruptMsg };
                }
                terminate_child();
                return format_sys_error(read_result.error().value(), "Failed to read from pipe");
            }

            const std::size_t bytes = *read_result;
            if (bytes == 0) { break; }

            if (total_read + bytes <= max_output_size) {
                result.output.append(buffer.data(), bytes);
                total_read += bytes;
                continue;
            }

            const std::size_t remaining_space = max_output_size - total_read;
            if (remaining_space > 0) { result.output.append(buffer.data(), remaining_space); }
            result.output.append("\n[Output truncated (too large)]");
            terminate_child();
            break;
        }
        return std::nullopt;
    }();

    if (read_error) {
        result.error = *read_error;
        if (is_stopped()) { result.interrupted = true; }
    }

    read_fd_.reset();
    if (pid_ == -1) { return result; }

    const scope_exit reap_guard { [this]() noexcept { terminate_child(); } };

    if (is_stopped() || result.interrupted) {
        result.interrupted = true;
        if (result.error.empty()) { result.error = std::string { config::kInterruptMsg }; }
        return result;
    }

    const auto reap_error = [this, raise_on_error, &result]() -> std::optional<std::string> {
        const auto wait_res = posix::waitpid(pid_, 0);
        if (!wait_res) { return format_sys_error(wait_res.error().value(), "waitpid failed for child process"); }
        if (!wait_res->has_value()) { return "waitpid returned no status unexpectedly"; }

        const auto& ws = wait_res->value();
        if (ws.signaled()) { return ws.describe_signal(); }

        if (ws.exited() && ws.exit_code() != 0) {
            result.exit_code = ws.exit_code();
            if (result.output.empty() || raise_on_error) {
                return std::format("Child exited with code {}", result.exit_code);
            }
        }
        return std::nullopt;
    }();

    if (reap_error) {
        result.error = !result.output.empty() ? std::format("{}\nOutput: {}", *reap_error, result.output) : *reap_error;
    }

    return result;
}

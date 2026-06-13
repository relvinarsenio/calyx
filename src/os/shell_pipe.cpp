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
#include "scope.hpp"
#include "utils.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <spawn.h>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace chrono = std::chrono;

namespace sp_impl {

/**
 * @brief Helper to check readability of the pipe using poll.
 */
[[nodiscard]] auto poll_ready(const posix::file_descriptor& fd, chrono::steady_clock::time_point deadline) noexcept
    -> std::expected<bool, std::error_code> {
    const auto timeout_ms = posix::sys_helpers::compute_poll_timeout_ms(false, deadline);
    if (timeout_ms <= 0) { return false; }

    std::array<pollfd, 1uz> pfds { pollfd { .fd = fd.native_handle(), .events = POLLIN, .revents = 0 } };
    const auto poll_res = posix::poll(pfds, chrono::milliseconds(timeout_ms));
    if (!poll_res) { return std::unexpected(poll_res.error()); }
    return *poll_res > 0;
}

/**
 * @brief Reads output from the pipe until EOF, timeout, or interruption.
 */
[[nodiscard]] auto read_pipe_output(const posix::file_descriptor& read_fd, chrono::steady_clock::time_point deadline,
    const auto& is_stopped, ShellPipeResult& result) -> std::optional<std::string> {
    const std::size_t max_output_size = config::kPipeMaxOutputBytes;
    std::array<char, config::kPipeBufferSize> buffer {};
    std::size_t total_read = 0;
    bool truncated         = false;

    while (true) {
        if (is_stopped()) { return std::string { config::kInterruptMsg }; }

        const auto ready = poll_ready(read_fd, deadline);
        if (!ready) { return format_sys_error(ready.error(), "poll failed on child output"); }
        if (!*ready) { return "Child process timed out while reading output"; }

        const auto read_res = read_fd.read(std::as_writable_bytes(std::span { buffer }), is_stopped);
        if (!read_res) {
            return read_res.error() == std::make_error_code(std::errc::operation_canceled)
                ? std::string { config::kInterruptMsg }
                : format_sys_error(read_res.error(), "Failed to read from pipe");
        }

        const std::size_t bytes = *read_res;
        if (bytes == 0) { break; }

        if (truncated) { continue; }

        const auto new_total = safe_add(total_read, bytes);
        if (new_total && *new_total <= max_output_size) {
            result.output.append(buffer.data(), bytes);
            total_read = *new_total;
            continue;
        }

        const auto remaining = safe_sub(max_output_size, total_read).value_or(0uz);
        if (remaining > 0) { result.output.append(buffer.data(), remaining); }
        result.output.append("\n[Output truncated (too large)]");
        truncated = true;
    }
    return std::nullopt;
}

/**
 * @brief Polls waitpid for a specific process ID until it exits, deadline is reached, or stopped.
 *
 * Unifies the non-blocking waitpid polling loop to comply with DRY principles.
 */
[[nodiscard]] auto poll_waitpid(std::int32_t pid, chrono::steady_clock::time_point deadline,
    const auto& is_stopped) noexcept -> std::expected<std::optional<posix::wait_status>, std::error_code> {
    while (true) {
        const auto wait_res = posix::waitpid(pid, WNOHANG);
        if (wait_res && wait_res->has_value()) { return wait_res; }
        if (!wait_res && wait_res.error() == std::errc::no_child_process) { return std::unexpected(wait_res.error()); }
        if (!wait_res) { return std::unexpected(wait_res.error()); }

        if (is_stopped()) { return std::unexpected(std::make_error_code(std::errc::operation_canceled)); }

        if (posix::sys_helpers::poll_deadline_reached(false, deadline)) { return std::nullopt; }

        std::this_thread::sleep_for(chrono::milliseconds(config::kShellPipePollIntervalMs));
    }
}

/**
 * @brief Waits for the child process to exit or handles timeout/interruption.
 */
[[nodiscard]] auto wait_for_child(child_process& process, chrono::steady_clock::time_point deadline,
    const auto& is_stopped, const auto& terminate_fn) -> std::expected<posix::wait_status, std::error_code> {
    scope_exit release_guard { [&process]() noexcept { process.release(); } };

    const auto res = poll_waitpid(process.native_handle(), deadline, is_stopped);
    if (!res) {
        if (res.error() == std::errc::operation_canceled) {
            release_guard.release();
            terminate_fn();
        }
        return std::unexpected(res.error());
    }
    if (!res->has_value()) {
        release_guard.release();
        terminate_fn();
        return std::unexpected(std::make_error_code(std::errc::timed_out));
    }
    return **res;
}

/**
 * @brief Attempts to terminate a process using progressive signaling and reaping.
 */
auto try_terminate(std::int32_t pid, posix::signal sig, chrono::milliseconds wait_time) noexcept
    -> std::expected<void, std::error_code> {
    const auto res = posix::kill(pid, sig);
    if (!res && res.error() == std::errc::no_such_process) { return {}; }
    if (!res) {
        print_warning(format_sys_error(res.error(), "kill failed"));
        return std::unexpected(res.error());
    }

    const auto deadline = chrono::steady_clock::now() + wait_time;
    const auto wait_res = poll_waitpid(pid, deadline, []() noexcept { return false; });
    if (wait_res && wait_res->has_value()) { return {}; }
    if (!wait_res && wait_res.error() == std::errc::no_child_process) { return {}; }
    return std::unexpected(wait_res ? std::make_error_code(std::errc::timed_out) : wait_res.error());
}

/**
 * @brief Parses child exit wait status and records exit codes or signals.
 */
void handle_child_exit(const posix::wait_status& ws, bool raise_on_error, ShellPipeResult& result) {
    if (ws.signaled()) {
        result.error = ws.describe_signal();
        return;
    }
    if (ws.exited() && ws.exit_code() != 0) {
        result.exit_code = ws.exit_code();
        if (result.output.empty() || raise_on_error) {
            result.error = std::format("Child exited with code {}", result.exit_code);
        }
    }
}

/**
 * @brief Maps child wait errors to user-facing diagnostic messages.
 */
void handle_wait_error(std::error_code ec, ShellPipeResult& result) {
    if (ec == std::errc::operation_canceled) {
        result.interrupted = true;
        result.error       = std::string { config::kInterruptMsg };
    } else if (ec == std::errc::timed_out) {
        result.error = "Child process timed out waiting for exit status";
    } else if (ec != std::errc::no_child_process) {
        result.error = format_sys_error(ec, "waitpid failed for child process");
    }
}

/** @brief Inits file actions to redirect stdout and stderr to the pipe write end. */
[[nodiscard]] auto configure_spawn_file_actions(posix_spawn_file_actions_t& actions,
    posix::file_descriptor::native_handle_type write_fd) noexcept -> std::expected<void, std::error_code> {
    return posix::expect_success<posix::error_style::pthreads>(posix_spawn_file_actions_init(&actions))
        .and_then([&actions, write_fd]() noexcept -> std::expected<void, std::error_code> {
            posix_spawn_file_actions_adddup2(&actions, write_fd, STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, write_fd, STDERR_FILENO);
            return {};
        });
}

/** @brief Inits spawn attributes with an empty signal mask and SIG_DFL for all dispositions. */
[[nodiscard]] auto configure_spawn_attr(posix_spawnattr_t& attr) noexcept -> std::expected<void, std::error_code> {
    return posix::expect_success<posix::error_style::pthreads>(posix_spawnattr_init(&attr))
        .and_then([&attr]() noexcept -> std::expected<void, std::error_code> {
            sigset_t empty, all;
            sigemptyset(&empty);
            sigfillset(&all);
            posix_spawnattr_setsigmask(&attr, &empty);
            posix_spawnattr_setsigdefault(&attr, &all);
            posix_spawnattr_setflags(&attr, static_cast<short>(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
            return {};
        });
}

/**
 * @brief Spawns a child process with stdout/stderr redirected to the pipe and a clean signal state.
 *
 * posix_spawn(3) atomically applies file actions and spawn attributes without any
 * post-fork user-space code, eliminating the UB risk that arises from calling
 * non-async-signal-safe functions in a multi-threaded parent after fork().
 *
 * The pipe fds carry O_CLOEXEC so exec() closes them automatically; only the
 * dup2’d stdout/stderr targets (no CLOEXEC) remain open in the child.
 */
[[nodiscard]] auto spawn_child(const std::filesystem::path& path, const std::vector<char*>& argv,
    posix::file_descriptor::native_handle_type write_fd) noexcept -> std::expected<pid_t, std::error_code> {
    posix_spawn_file_actions_t actions;
    if (const auto res = configure_spawn_file_actions(actions, write_fd); !res) { return std::unexpected(res.error()); }
    scope_exit destroy_actions { [&actions]() noexcept { posix_spawn_file_actions_destroy(&actions); } };

    posix_spawnattr_t attr;
    if (const auto res = configure_spawn_attr(attr); !res) { return std::unexpected(res.error()); }
    scope_exit destroy_attr { [&attr]() noexcept { posix_spawnattr_destroy(&attr); } };

    pid_t child_pid = -1;
    return posix::expect_success<posix::error_style::pthreads>(
        posix_spawn(&child_pid, path.c_str(), &actions, &attr, argv.data(), ::environ))
        .transform([child_pid]() noexcept -> pid_t { return child_pid; });
}

/**
 * @brief Prepares a null-terminated argument vector referencing strings in args.
 *
 * This isolates the argv construction required by execv-family system calls to prevent
 * memory management issues and ensure lifetime compatibility.
 */
[[nodiscard]] auto prepare_argv(std::vector<std::string>& args) noexcept -> std::vector<char*> {
    auto argv = args | std::views::transform([](std::string& s) noexcept { return s.data(); })
        | std::ranges::to<std::vector<char*>>();
    argv.push_back(nullptr);
    return argv;
}

/**
 * @brief Reaps the child process, waiting for termination and parsing the exit status.
 *
 * This encapsulates the wait loop and diagnostic error mapping to ensure the parent
 * does not leak zombie processes or misreport execution failures.
 */
void reap_child_process(child_process& process, chrono::steady_clock::time_point outer_deadline, const auto& is_stopped,
    bool raise_on_error, ShellPipeResult& result) {
    if (!process) { return; }

    auto wait_res = wait_for_child(process, outer_deadline, is_stopped, [&process]() noexcept { process.reset(); });

    if (!wait_res && wait_res.error() == std::errc::timed_out) {
        const auto term_deadline = chrono::steady_clock::now() + chrono::milliseconds(config::kShellPipeTermWaitMs);
        wait_res = wait_for_child(process, term_deadline, is_stopped, [&process]() noexcept { process.reset(); });
    }

    if (!wait_res) {
        handle_wait_error(wait_res.error(), result);
    } else {
        handle_child_exit(*wait_res, raise_on_error, result);
    }
}

} // namespace sp_impl

void sp_impl::child_process::reset(std::int32_t new_pid) noexcept {
    if (posix::expect_result<posix::error_style::posix>(pid_)) { terminate(); }
    pid_ = new_pid;
}

void sp_impl::child_process::terminate() noexcept {
    /**
     * @brief Progressively terminate the process, attempting SIGTERM and falling back to SIGKILL.
     * @details If either signaling attempt successfully reaps the child, reset the PID to -1.
     */
    sp_impl::try_terminate(pid_, posix::signal::Term, chrono::milliseconds(config::kShellPipeTermWaitMs))
        .or_else([this](std::error_code) noexcept {
            return sp_impl::try_terminate(pid_, posix::signal::Kill, chrono::seconds(config::kShellPipeKillWaitSec));
        })
        .transform([this]() noexcept { pid_ = -1; });
}

auto ShellPipe::create(std::vector<std::string> args) -> std::expected<ShellPipe, std::error_code> {
    if (args.empty()) [[unlikely]] { return std::unexpected(std::make_error_code(std::errc::invalid_argument)); }

    auto resolved_exec = posix::resolve_executable(args[0]);
    if (!resolved_exec) { return std::unexpected(resolved_exec.error()); }

    /**
     * @brief Guard to ensure the parent's copy of the executable file descriptor is closed.
     * @details Prevents descriptor leakage in the parent process upon function exit.
     */
    scope_exit close_exec { [&resolved_exec]() noexcept { resolved_exec->fd.reset(); } };

    const auto argv = sp_impl::prepare_argv(args);

    auto pipe_result = posix::pipe::create();
    if (!pipe_result) { return std::unexpected(pipe_result.error()); }

    ShellPipe self;
    self.read_fd_ = pipe_result->release_read();
    auto write_fd = pipe_result->release_write();

    /**
     * @brief Guard to ensure the parent's copy of the write-end pipe descriptor is closed.
     * @details Ensures the parent does not hold an open writer, which would cause read loops
     *          to block indefinitely waiting for EOF.
     */
    scope_exit close_write { [&write_fd]() noexcept { write_fd.reset(); } };

    const auto spawn_res = sp_impl::spawn_child(resolved_exec->path, argv, write_fd.native_handle());
    if (!spawn_res) { return std::unexpected(spawn_res.error()); }

    self.pid_.reset(*spawn_res);
    return self;
}

[[nodiscard]] ShellPipeResult ShellPipe::read_all(
    chrono::milliseconds timeout, std::stop_token stop, bool raise_on_error) {
    ShellPipeResult result;
    const auto is_stopped = [&stop]() noexcept { return check_interrupted() || stop.stop_requested(); };

    scope_exit reap_guard { [this]() noexcept { pid_.reset(); } };
    scope_exit close_read { [this]() noexcept { read_fd_.reset(); } };

    const auto deadline = chrono::steady_clock::now() + timeout;
    const auto read_err = sp_impl::read_pipe_output(read_fd_, deadline, is_stopped, result);

    if (read_err) {
        result.error = *read_err;
        if (is_stopped()) { result.interrupted = true; }
        return result;
    }

    sp_impl::reap_child_process(pid_, deadline, is_stopped, raise_on_error, result);

    return result;
}

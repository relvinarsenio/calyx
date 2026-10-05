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
#include "numeric_cast.hpp"
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
#include <utility>
#include <vector>

namespace chrono = std::chrono;

namespace {

/**
 * @brief Determines whether child output is readable before the deadline.
 */
[[nodiscard]] auto poll_ready(const posix::file_descriptor& fd, chrono::steady_clock::time_point deadline) noexcept
    -> std::expected<bool, std::error_code> {
    const auto now = chrono::steady_clock::now();
    if (now >= deadline) { return false; }

    const auto timeout = chrono::duration_cast<chrono::milliseconds>(deadline - now);
    std::array<pollfd, 1uz> pfds { pollfd { .fd = fd.native_handle(), .events = POLLIN, .revents = 0 } };
    const auto poll_res = posix::poll(pfds, timeout);
    if (!poll_res) { return std::unexpected(poll_res.error()); }
    return *poll_res > 0;
}

/**
 * @brief Captures child output until completion, timeout, or interruption.
 */
[[nodiscard]] auto read_pipe_output(const posix::file_descriptor& read_fd, chrono::steady_clock::time_point deadline,
    const auto& is_stopped, ShellPipeResult& result) -> bool {
    const std::size_t max_output_size = config::kPipeMaxOutputBytes;
    std::array<char, config::kPipeBufferSize> buffer {};
    std::size_t total_read = 0;

    while (true) {
        if (is_stopped()) {
            result.status = ShellPipeStatus::interrupted;
            result.error  = std::string { config::kInterruptMsg };
            return false;
        }

        const auto ready = poll_ready(read_fd, deadline);
        if (!ready) {
            result.status = ShellPipeStatus::error;
            result.error  = format_sys_error(ready.error(), "poll failed on child output");
            return false;
        }
        if (!*ready) {
            result.status = ShellPipeStatus::timed_out;
            result.error  = "Child process timed out while reading output";
            return false;
        }

        const auto read_res = read_fd.read(std::as_writable_bytes(std::span { buffer }), is_stopped);
        if (!read_res) {
            const auto ec       = read_res.error();
            const bool canceled = (ec == std::make_error_code(std::errc::operation_canceled));
            result.status       = canceled ? ShellPipeStatus::interrupted : ShellPipeStatus::error;
            result.error
                = canceled ? std::string { config::kInterruptMsg } : format_sys_error(ec, "Failed to read from pipe");
            return false;
        }

        const std::size_t bytes = *read_res;
        if (bytes == 0) { break; }

        if (result.truncated) { continue; }

        const auto new_total = safe_add(total_read, bytes);
        if (new_total && *new_total <= max_output_size) {
            result.output.append(buffer.data(), bytes);
            total_read = *new_total;
            continue;
        }

        const auto remaining = safe_sub(max_output_size, total_read).value_or(0uz);
        if (remaining > 0) { result.output.append(buffer.data(), remaining); }
        result.output.append("\n[Output truncated (too large)]");
        result.truncated = true;
    }
    return true;
}

/**
 * @brief Observes child termination until completion, timeout, or interruption.
 *
 * Uses non-blocking status checks so cancellation and the deadline remain
 * observable while the child is still running.
 */
[[nodiscard]] auto poll_waitpid(pid_t pid, chrono::steady_clock::time_point deadline, const auto& is_stopped) noexcept
    -> std::expected<std::optional<posix::wait_status>, std::error_code> {
    while (true) {
        const auto wait_res = posix::waitpid(pid, WNOHANG);
        if (wait_res && wait_res->has_value()) { return wait_res; }
        if (!wait_res && wait_res.error() == std::errc::no_child_process) { return std::unexpected(wait_res.error()); }
        if (!wait_res) { return std::unexpected(wait_res.error()); }

        if (is_stopped()) { return std::unexpected(std::make_error_code(std::errc::operation_canceled)); }

        if (posix::sys_helpers::poll_deadline_reached(false, deadline)) { return std::nullopt; }

        std::this_thread::sleep_for(config::kShellPipePollInterval);
    }
}

/**
 * @brief Ensures a child process does not outlive its owner.
 */
auto try_terminate(pid_t pid, posix::signal sig, chrono::milliseconds wait_time) noexcept
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

class ChildProcess {
    pid_t pid_ = -1;

    void terminate() noexcept {
        try_terminate(pid_, posix::signal::Term, config::kShellPipeTermWait)
            .or_else([this](std::error_code) noexcept {
                return try_terminate(pid_, posix::signal::Kill, config::kShellPipeKillWait);
            })
            .transform([this]() noexcept { pid_ = -1; });
    }

public:
    ChildProcess() noexcept = default;
    explicit ChildProcess(pid_t pid) noexcept
        : pid_(pid) {}
    ~ChildProcess() noexcept { reset(); }

    ChildProcess(const ChildProcess&)            = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ChildProcess(ChildProcess&& other) noexcept
        : pid_(std::exchange(other.pid_, -1)) {}

    ChildProcess& operator=(ChildProcess&& other) noexcept {
        if (this != &other) {
            reset();
            pid_ = std::exchange(other.pid_, -1);
        }
        return *this;
    }

    [[nodiscard]] auto native_handle() const noexcept -> pid_t { return pid_; }
    auto release() noexcept -> pid_t { return std::exchange(pid_, -1); }

    void reset(pid_t new_pid = -1) noexcept {
        if (posix::expect_result<posix::error_style::posix>(pid_)) { terminate(); }
        pid_ = new_pid;
    }

    explicit operator bool() const noexcept {
        return posix::expect_result<posix::error_style::posix>(pid_).has_value();
    }
};

/**
 * @brief Waits for child completion while preserving timeout and cancellation results.
 */
[[nodiscard]] auto wait_for_child(ChildProcess& process, chrono::steady_clock::time_point deadline,
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

/** @brief Converts a child termination status into the public execution result. */
void handle_child_exit(const posix::wait_status& ws, bool raise_on_error, ShellPipeResult& result) {
    if (ws.signaled()) {
        result.status = ShellPipeStatus::signaled;
        result.signal = ws.term_signal();
        result.error  = ws.describe_signal();
        return;
    }
    if (ws.exited() && ws.exit_code() != 0) {
        result.status    = ShellPipeStatus::nonzero_exit;
        result.exit_code = ws.exit_code();
        if (result.output.empty() || raise_on_error) {
            result.error = std::format("Child exited with code {}", result.exit_code);
        }
    }
}

/** @brief Converts child lifecycle errors into the public execution result. */
void handle_wait_error(std::error_code ec, ShellPipeResult& result) {
    if (ec == std::errc::operation_canceled) {
        result.status = ShellPipeStatus::interrupted;
        result.error  = std::string { config::kInterruptMsg };
    } else if (ec == std::errc::timed_out) {
        result.status = ShellPipeStatus::timed_out;
        result.error  = "Child process timed out waiting for exit status";
    } else if (ec != std::errc::no_child_process) {
        result.status = ShellPipeStatus::error;
        result.error  = format_sys_error(ec, "waitpid failed for child process");
    }
}

/**
 * @brief Configures stdout and stderr capture for the spawned child.
 */
[[nodiscard]] auto configure_spawn_file_actions(posix_spawn_file_actions_t& actions,
    posix::file_descriptor::native_handle_type write_fd) noexcept -> std::expected<void, std::error_code> {
    auto res = posix::expect_success<posix::error_style::pthreads>(posix_spawn_file_actions_init(&actions));
    if (!res) { return res; }

    scope_exit guard { [&actions]() noexcept { posix_spawn_file_actions_destroy(&actions); } };

    res = posix::expect_success<posix::error_style::pthreads>(
        posix_spawn_file_actions_adddup2(&actions, write_fd, STDOUT_FILENO));
    if (!res) { return res; }

    res = posix::expect_success<posix::error_style::pthreads>(
        posix_spawn_file_actions_adddup2(&actions, write_fd, STDERR_FILENO));
    if (!res) { return res; }

    guard.release();
    return {};
}

/**
 * @brief Gives the spawned child an independent signal state.
 */
[[nodiscard]] auto configure_spawn_attr(posix_spawnattr_t& attr) noexcept -> std::expected<void, std::error_code> {
    auto res = posix::expect_success<posix::error_style::pthreads>(posix_spawnattr_init(&attr));
    if (!res) { return res; }

    scope_exit guard { [&attr]() noexcept { posix_spawnattr_destroy(&attr); } };

    sigset_t empty {}, all {};
    sigemptyset(&empty);
    sigfillset(&all);

    res = posix::expect_success<posix::error_style::pthreads>(posix_spawnattr_setsigmask(&attr, &empty));
    if (!res) { return res; }
    res = posix::expect_success<posix::error_style::pthreads>(posix_spawnattr_setsigdefault(&attr, &all));
    if (!res) { return res; }
    res = posix::expect_success<posix::error_style::pthreads>(
        posix_spawnattr_setflags(&attr, toShort(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF)));
    if (!res) { return res; }

    guard.release();
    return {};
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
    posix_spawn_file_actions_t actions {};
    if (const auto res = configure_spawn_file_actions(actions, write_fd); !res) { return std::unexpected(res.error()); }
    scope_exit destroy_actions { [&actions]() noexcept { posix_spawn_file_actions_destroy(&actions); } };

    posix_spawnattr_t attr {};
    if (const auto res = configure_spawn_attr(attr); !res) { return std::unexpected(res.error()); }
    scope_exit destroy_attr { [&attr]() noexcept { posix_spawnattr_destroy(&attr); } };

    pid_t child_pid = -1;
    return posix::expect_success<posix::error_style::pthreads>(
        posix_spawn(&child_pid, path.c_str(), &actions, &attr, argv.data(), ::environ))
        .transform([child_pid]() noexcept -> pid_t { return child_pid; });
}

/**
 * @brief Builds the argument vector required by the process launcher.
 *
 * The launcher requires a null-terminated array of mutable string pointers.
 * The returned pointers remain valid while the caller-owned argument strings
 * remain alive.
 */
[[nodiscard]] auto prepare_argv(std::vector<std::string>& args) -> std::vector<char*> {
    auto argv = args | std::views::transform([](std::string& s) noexcept { return s.data(); })
        | std::ranges::to<std::vector<char*>>();
    argv.push_back(nullptr);
    return argv;
}

/**
 * @brief Finalizes child lifetime and records its termination result.
 *
 * Centralizes reaping and status translation so every exit path leaves no
 * zombie process behind and exposes only `ShellPipeResult` to callers.
 */
void reap_child_process(ChildProcess& process, chrono::steady_clock::time_point outer_deadline, const auto& is_stopped,
    bool raise_on_error, ShellPipeResult& result) {
    if (!process) { return; }

    const auto wait_res
        = wait_for_child(process, outer_deadline, is_stopped, [&process]() noexcept { process.reset(); });

    if (!wait_res) {
        handle_wait_error(wait_res.error(), result);
    } else {
        handle_child_exit(*wait_res, raise_on_error, result);
    }
}

} // namespace

class ShellPipe::Impl {
public:
    posix::file_descriptor read_fd;
    ChildProcess child;
};

ShellPipe::ShellPipe(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ShellPipe::~ShellPipe() noexcept = default;

ShellPipe::ShellPipe(ShellPipe&&) noexcept = default;

ShellPipe& ShellPipe::operator=(ShellPipe&&) noexcept = default;

auto ShellPipe::create(std::vector<std::string> args) -> std::expected<ShellPipe, std::error_code> {
    if (args.empty()) [[unlikely]] { return std::unexpected(std::make_error_code(std::errc::invalid_argument)); }

    auto resolved_exec = posix::resolve_executable(args[0]);
    if (!resolved_exec) { return std::unexpected(resolved_exec.error()); }

    /**
     * @brief Guard to ensure the parent's copy of the executable file descriptor is closed.
     * @details Prevents descriptor leakage in the parent process upon function exit.
     */
    scope_exit close_exec { [&resolved_exec]() noexcept { resolved_exec->fd.reset(); } };

    const auto argv = prepare_argv(args);

    auto pipe_result = posix::pipe::create();
    if (!pipe_result) { return std::unexpected(pipe_result.error()); }

    auto impl     = std::make_unique<Impl>();
    impl->read_fd = pipe_result->release_read();
    auto write_fd = pipe_result->release_write();

    /**
     * @brief Guard to ensure the parent's copy of the write-end pipe descriptor is closed.
     * @details Ensures the parent does not hold an open writer, which would cause read loops
     *          to block indefinitely waiting for EOF.
     */
    scope_exit close_write { [&write_fd]() noexcept { write_fd.reset(); } };

    const auto spawn_res = spawn_child(resolved_exec->path, argv, write_fd.native_handle());
    if (!spawn_res) { return std::unexpected(spawn_res.error()); }

    impl->child.reset(*spawn_res);
    return ShellPipe { std::move(impl) };
}

[[nodiscard]] ShellPipeResult ShellPipe::read_all(chrono::milliseconds timeout, std::stop_token stop,
    std::move_only_function<bool() const noexcept> interrupt_cb, bool raise_on_error) {
    ShellPipeResult result {};
    if (!impl_) {
        result.status = ShellPipeStatus::error;
        result.error  = "ShellPipe has no active process";
        return result;
    }

    const auto is_stopped
        = [&stop, &interrupt_cb]() noexcept { return stop.stop_requested() || (interrupt_cb && interrupt_cb()); };

    scope_exit reap_guard { [this]() noexcept { impl_->child.reset(); } };
    scope_exit close_read { [this]() noexcept { impl_->read_fd.reset(); } };

    const auto read_deadline = chrono::steady_clock::now() + timeout;
    if (!read_pipe_output(impl_->read_fd, read_deadline, is_stopped, result)) { return result; }

    const auto reap_deadline = chrono::steady_clock::now() + config::kShellPipeTermWait;
    reap_child_process(impl_->child, reap_deadline, is_stopped, raise_on_error, result);

    return result;
}

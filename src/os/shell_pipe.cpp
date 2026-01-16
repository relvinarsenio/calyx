/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/shell_pipe.hpp"
#include "include/config.hpp"
#include "include/interrupts.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

static std::string describe_signal(int sig) {
    switch (sig) {
        case SIGINT:
            return "Interrupted by user (SIGINT)";
        case SIGTERM:
            return "Terminated (SIGTERM)";
        case SIGKILL:
            return "Killed (SIGKILL)";
        case SIGQUIT:
            return "Quit (SIGQUIT)";
        case SIGPIPE:
            return "Broken pipe (SIGPIPE)";
        case SIGHUP:
            return "Hangup (SIGHUP)";
        case SIGABRT:
            return "Aborted (SIGABRT)";
        case SIGSEGV:
            return "Segmentation fault (SIGSEGV)";
        default: {
            const char* msg = ::strsignal(sig);
            if (msg) {
                return std::string("Child terminated by signal ") + std::to_string(sig) + " (" +
                       msg + ")";
            }
            return std::string("Child terminated by signal ") + std::to_string(sig);
        }
    }
}

ShellPipe::ShellPipe(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::invalid_argument("ShellPipe: Empty argument list");
    }

    std::vector<std::string> args_copy = args;
    std::vector<char*> c_args;
    c_args.reserve(args_copy.size() + 1);

    for (auto& arg : args_copy) {
        c_args.push_back(arg.data());
    }
    c_args.push_back(nullptr);

    int pipe_fds[2];
    if (::pipe(pipe_fds) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to create pipe");
    }

    read_fd_.reset(pipe_fds[0]);
    FileDescriptor write_fd(pipe_fds[1]);

    pid_t pid = ::fork();
    if (pid == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to fork process");
    }

    if (pid == 0) {
        if (::dup2(write_fd.get(), STDOUT_FILENO) == -1)
            ::_exit(errno);
        if (::dup2(write_fd.get(), STDERR_FILENO) == -1)
            ::_exit(errno);

        read_fd_.reset();
        write_fd.reset();

        ::execvp(c_args[0], c_args.data());

        const char* msg = "Failed to execute binary\n";
        [[maybe_unused]] auto val = ::write(STDOUT_FILENO, msg, std::strlen(msg));

        ::_exit(127);
    }

    pid_ = pid;
}

ShellPipe::~ShellPipe() noexcept {
    read_fd_.reset();

    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);

        if (::waitpid(pid_, nullptr, WNOHANG) != pid_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(32));

            if (::waitpid(pid_, nullptr, WNOHANG) != pid_) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
        }
        pid_ = -1;
    }
}

std::string ShellPipe::read_all(std::chrono::milliseconds timeout,
                                std::stop_token stop,
                                bool raise_on_error) {
    std::string output;
    std::array<char, 4096> buffer;
    size_t total_read = 0;
    const size_t MAX_OUTPUT_SIZE = Config::PIPE_MAX_OUTPUT_BYTES;

    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        if (g_interrupted || stop.stop_requested())
            break;

        auto now = std::chrono::steady_clock::now();
        int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remaining_ms <= 0) {
            ::kill(pid_, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int status;
            if (::waitpid(pid_, &status, WNOHANG) == 0) {
                ::kill(pid_, SIGKILL);
            }

            ::waitpid(pid_, &status, 0);
            pid_ = -1;

            read_fd_.reset();

            throw std::runtime_error("Child process timed out while reading output");
        }

        struct pollfd pfd {};
        pfd.fd = read_fd_.get();
        pfd.events = POLLIN;

        int poll_res = ::poll(&pfd, 1, remaining_ms);
        if (poll_res == -1) {
            if (errno == EINTR) {
                if (g_interrupted || stop.stop_requested())
                    break;
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "poll failed on child output");
        }

        if (poll_res == 0) {
            continue;
        }

        ssize_t bytes_read = ::read(read_fd_.get(), buffer.data(), buffer.size());

        if (bytes_read > 0) {
            if (total_read + static_cast<size_t>(bytes_read) > MAX_OUTPUT_SIZE) {
                output += "\n[Output truncated (too large)]";
                break;
            }
            output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            total_read += static_cast<std::size_t>(bytes_read);
        } else if (bytes_read == 0) {
            break;
        } else {
            if (errno == EINTR) {
                if (g_interrupted || stop.stop_requested())
                    break;
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "Failed to read from pipe");
        }
    }

    read_fd_.reset();

    if (pid_ != -1) {
        if (g_interrupted || stop.stop_requested()) {
            ::kill(pid_, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            int temp_status = 0;
            if (::waitpid(pid_, &temp_status, WNOHANG) == 0) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &temp_status, 0);
            }

            pid_ = -1;
            throw std::runtime_error("Operation interrupted by user");
        }

        int status = 0;
        if (::waitpid(pid_, &status, 0) == -1) {
            throw std::system_error(
                errno, std::generic_category(), "waitpid failed for child process");
        }

        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            pid_ = -1;
            throw std::runtime_error(describe_signal(sig));
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            int code = WEXITSTATUS(status);
            pid_ = -1;
            if (output.empty() || raise_on_error) {
                std::string msg = "Child exited with code " + std::to_string(code);
                if (!output.empty())
                    msg += "\nOutput: " + output;
                throw std::runtime_error(msg);
            }
            return output;
        }

        pid_ = -1;
    }

    return output;
}
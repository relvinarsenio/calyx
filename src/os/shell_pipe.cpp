#include "include/shell_pipe.hpp"
#include "include/interrupts.hpp"
#include "include/config.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>

#include <fcntl.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>
#include <poll.h>

static int pidfd_open(pid_t pid, unsigned int flags) {
#ifdef __NR_pidfd_open
    return static_cast<int>(syscall(__NR_pidfd_open, pid, flags));
#else
    errno = ENOSYS;
    return -1;
#endif
}

static std::string describe_signal(int sig) {
    switch (sig) {
        case SIGINT:  return "Interrupted by user (SIGINT)";
        case SIGTERM: return "Terminated (SIGTERM)";
        case SIGKILL: return "Killed (SIGKILL)";
        case SIGQUIT: return "Quit (SIGQUIT)";
        case SIGPIPE: return "Broken pipe (SIGPIPE)";
        case SIGHUP:  return "Hangup (SIGHUP)";
        case SIGABRT: return "Aborted (SIGABRT)";
        case SIGSEGV: return "Segmentation fault (SIGSEGV)";
        default: {
            const char* msg = ::strsignal(sig);
            if (msg) {
                return std::string("Child terminated by signal ") + std::to_string(sig) + " (" + msg + ")";
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

    pid_t pid = ::fork();
    if (pid == -1) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        throw std::system_error(errno, std::generic_category(), "Failed to fork process");
    }

    if (pid == 0) {
        if (::dup2(pipe_fds[1], STDOUT_FILENO) == -1) ::_exit(errno);
        if (::dup2(pipe_fds[1], STDERR_FILENO) == -1) ::_exit(errno);

        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);

        ::execvp(c_args[0], c_args.data());

        const char* msg = "Failed to execute binary\n";
        [[maybe_unused]] auto val = ::write(STDOUT_FILENO, msg, std::strlen(msg));
        
        ::_exit(127);
    }

    ::close(pipe_fds[1]);
    read_fd_ = pipe_fds[0];
    pid_ = pid;
}

ShellPipe::~ShellPipe() {
    if (read_fd_ != -1) {
        ::close(read_fd_);
    }

    if (pid_ != -1) {
        int status;
        pid_t result = ::waitpid(pid_, &status, WNOHANG);
        
        if (result == pid_) {
            return;
        }

        ::kill(pid_, SIGTERM);
        
        bool reaped = false;
        int pfd = pidfd_open(pid_, 0);
        
        if (pfd >= 0) {
            struct pollfd pfd_struct;
            pfd_struct.fd = pfd;
            pfd_struct.events = POLLIN;

            int ret = ::poll(&pfd_struct, 1, 1000); 
            ::close(pfd);

            if (ret > 0) {
                ::waitpid(pid_, &status, 0); 
                reaped = true;
            }
        } 
        
        if (!reaped) {
            for (int i = 0; i < 5; ++i) {
                if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (!reaped) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
        }
    }
}

std::string ShellPipe::read_all(std::chrono::milliseconds timeout, std::stop_token stop, bool raise_on_error) {
    std::string output;
    std::array<char, 4096> buffer;
    size_t total_read = 0;
    const size_t MAX_OUTPUT_SIZE = Config::PIPE_MAX_OUTPUT_BYTES;

    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        if (g_interrupted || stop.stop_requested()) break;

        auto now = std::chrono::steady_clock::now();
        int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remaining_ms <= 0) {
            ::kill(pid_, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ::kill(pid_, SIGKILL);

            int status = 0;
            ::waitpid(pid_, &status, 0);
            pid_ = -1;

            if (read_fd_ != -1) {
                ::close(read_fd_);
                read_fd_ = -1;
            }

            throw std::runtime_error("Child process timed out while reading output");
        }

        struct pollfd pfd{};
        pfd.fd = read_fd_;
        pfd.events = POLLIN;

        int poll_res = ::poll(&pfd, 1, remaining_ms);
        if (poll_res == -1) {
            if (errno == EINTR) {
                if (g_interrupted || stop.stop_requested()) break;
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "poll failed on child output");
        }

        if (poll_res == 0) {
            // timeout hit, loop will terminate on next iteration deadline check
            continue;
        }

        ssize_t bytes_read = ::read(read_fd_, buffer.data(), buffer.size());

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
                if (g_interrupted || stop.stop_requested()) break;
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "Failed to read from pipe");
        }
    }

    if (read_fd_ != -1) {
        ::close(read_fd_);
        read_fd_ = -1;
    }

    if (pid_ != -1) {
        int status = 0;
        if (::waitpid(pid_, &status, 0) == -1) {
            throw std::system_error(errno, std::generic_category(), "waitpid failed for child process");
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
                if (!output.empty()) msg += "\nOutput: " + output;
                throw std::runtime_error(msg);
            }
            return output;
        }

        pid_ = -1;
    }

    return output;
}
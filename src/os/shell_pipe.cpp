#include "include/shell_pipe.hpp"
#include "include/interrupts.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <chrono>

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
        ::kill(pid_, SIGTERM);
        
        bool exited = false;
        for (int i = 0; i < 10; ++i) {
            int status;
            pid_t result = ::waitpid(pid_, &status, WNOHANG);
            
            if (result == pid_) {
                exited = true;
                break;
            }
            
            if (result == -1) {
                if (errno == EINTR) continue;
                exited = true; 
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!exited) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0); 
        }
    }
}

std::string ShellPipe::read_all() {
    std::string output;
    std::array<char, 4096> buffer;
    ssize_t bytes_read;
    size_t total_read = 0;
    const size_t MAX_OUTPUT_SIZE = 10 * 1024 * 1024;

    while (true) {
        if (g_interrupted) break;

        bytes_read = ::read(read_fd_, buffer.data(), buffer.size());
        
        if (bytes_read > 0) {
            if (total_read + bytes_read > MAX_OUTPUT_SIZE) {
                output += "\n[Output truncated (too large)]";
                ::kill(pid_, SIGTERM);
                break;
            }
            output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            total_read += static_cast<std::size_t>(bytes_read);
        } else if (bytes_read == 0) {
            break;
        } else {
            if (errno == EINTR) {
                if (g_interrupted) break;
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "Failed to read from pipe");
        }
    }

    int status;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;

    return output;
}

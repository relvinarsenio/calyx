#pragma once

#include <chrono>
#include <stop_token>
#include <string>
#include <vector>

class ShellPipe {
    int read_fd_ = -1;
    int pid_ = -1;

public:
    explicit ShellPipe(const std::vector<std::string>& args);
    
    ~ShellPipe();

    ShellPipe(const ShellPipe&) = delete;
    ShellPipe& operator=(const ShellPipe&) = delete;

    std::string read_all(std::chrono::milliseconds timeout = std::chrono::milliseconds(60000),
                         std::stop_token stop = {});
};
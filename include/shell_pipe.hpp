/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <chrono>
#include <stop_token>
#include <string>
#include <vector>

#include "file_descriptor.hpp"

class ShellPipe {
    FileDescriptor read_fd_;
    int pid_ = -1;

   public:
    explicit ShellPipe(const std::vector<std::string>& args);

    ~ShellPipe();

    ShellPipe(const ShellPipe&) = delete;
    ShellPipe& operator=(const ShellPipe&) = delete;

    std::string read_all(std::chrono::milliseconds timeout = std::chrono::milliseconds(60000),
                         std::stop_token stop = {},
                         bool raise_on_error = true);
};
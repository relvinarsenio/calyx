// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2025 Alfie Ardinata.

#include "include/file_descriptor.hpp"

#include <cerrno>
#include <unistd.h>

FileDescriptor::FileDescriptor(int fd) : fd_(fd) {
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "Invalid file descriptor");
    }
}

FileDescriptor::~FileDescriptor() {
    if (fd_ >= 0) ::close(fd_);
}

int FileDescriptor::get() const { return fd_; }

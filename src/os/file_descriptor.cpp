/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/file_descriptor.hpp"

#include <cerrno>
#include <unistd.h>

FileDescriptor::FileDescriptor(int fd) : fd_(fd) {
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "Invalid file descriptor");
    }
}

FileDescriptor::~FileDescriptor() {
    if (fd_ >= 0)
        ::close(fd_);
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

int FileDescriptor::get() const {
    return fd_;
}

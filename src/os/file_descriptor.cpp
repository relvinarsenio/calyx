/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/file_descriptor.hpp"

#include <unistd.h>
#include <cerrno>
#include <utility>

FileDescriptor::~FileDescriptor() {
    reset();
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
        reset(std::exchange(other.fd_, -1));
    }
    return *this;
}

void FileDescriptor::reset(int new_fd) {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = new_fd;
}

int FileDescriptor::release() {
    return std::exchange(fd_, -1);
}

std::expected<FileDescriptor, std::string> FileDescriptor::duplicate() const {
    if (fd_ < 0) {
        return std::unexpected("Cannot duplicate invalid file descriptor");
    }

    int new_fd = ::dup(fd_);
    if (new_fd < 0) {
        return std::unexpected(std::string("dup failed: ") + std::strerror(errno));
    }

    return FileDescriptor(new_fd);
}

void FileDescriptor::swap(FileDescriptor& other) noexcept {
    std::swap(fd_, other.fd_);
}

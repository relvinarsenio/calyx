/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <cerrno>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <unistd.h>

class FileDescriptor {
    int fd_ = -1;

   public:
    FileDescriptor() = default;

    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

    ~FileDescriptor() {
        reset();
    }

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    void reset(int new_fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
    }

    int release() {
        return std::exchange(fd_, -1);
    }

    [[nodiscard]] std::expected<FileDescriptor, std::string> duplicate() const {
        if (fd_ < 0) {
            return std::unexpected("Cannot duplicate invalid file descriptor");
        }

        int new_fd = ::dup(fd_);
        if (new_fd < 0) {
            return std::unexpected(std::format(
                "dup failed: {} (Code: {})", std::system_category().message(errno), errno));
        }

        return FileDescriptor(new_fd);
    }

    void swap(FileDescriptor& other) noexcept {
        std::swap(fd_, other.fd_);
    }

    int get() const noexcept {
        return fd_;
    }
    explicit operator bool() const noexcept {
        return fd_ >= 0;
    }
};

inline void swap(FileDescriptor& a, FileDescriptor& b) noexcept {
    a.swap(b);
}
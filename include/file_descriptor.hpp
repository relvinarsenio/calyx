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
#include <stdexcept>

class FileDescriptor {
    int fd_ = -1;

   public:
    FileDescriptor() = default;

    explicit FileDescriptor(int fd) : fd_(fd) {
        if (fd_ < 0 && fd != -1) [[unlikely]] {
            throw std::system_error(
                errno, std::generic_category(), "Failed to wrap invalid file descriptor");
        }
    }

    ~FileDescriptor() noexcept {
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

    void reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
    }

    int release() noexcept {
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

    [[nodiscard]] int get() const {
        if (fd_ < 0) [[unlikely]] {
            throw std::logic_error("FATAL: Accessing invalid file descriptor (-1)");
        }
        return fd_;
    }

    explicit operator bool() const noexcept {
        return fd_ >= 0;
    }
};

inline void swap(FileDescriptor& a, FileDescriptor& b) noexcept {
    a.swap(b);
}
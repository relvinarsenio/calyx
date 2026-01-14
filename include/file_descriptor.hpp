/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <expected>
#include <string>
#include <utility>

class FileDescriptor {
    int fd_ = -1;

   public:
    FileDescriptor() = default;

    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

    ~FileDescriptor();

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept;
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;

    void reset(int new_fd = -1);
    int release();
    [[nodiscard]] std::expected<FileDescriptor, std::string> duplicate() const;
    void swap(FileDescriptor& other) noexcept;

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
};

inline void swap(FileDescriptor& a, FileDescriptor& b) noexcept {
    a.swap(b);
}

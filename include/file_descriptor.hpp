// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2025 Alfie Ardinata.

#pragma once

#include <system_error>

class FileDescriptor {
    int fd_ = -1;
public:
    explicit FileDescriptor(int fd);
    ~FileDescriptor();
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const;
};

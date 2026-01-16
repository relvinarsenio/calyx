/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <cstdint>
#include "config.hpp"

namespace calyx::core {

enum class ExtractError {
    OpenFileFailed,
    ReadFailed,
    InvalidHeader,
    InvalidChecksum,
    CreateDirFailed,
    WriteFileFailed,
    PathTraversalDetected,
    FileTooLarge,
    ArchiveTooLarge,
    SymlinkDetected,
    UnicodeAttackDetected,
    DiskFull
};

class TgzExtractor {
   public:
    static std::expected<void, ExtractError> extract(const std::filesystem::path& tgz_path,
                                                     const std::filesystem::path& dest_dir);

    static std::string error_string(ExtractError err);
};

}  // namespace calyx::core
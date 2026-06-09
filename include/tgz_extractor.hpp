/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <expected>
#include <filesystem>
#include <string_view>

namespace archive {

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
    static std::expected<void, ExtractError> extract(
        const std::filesystem::path& tgz_path, const std::filesystem::path& dest_dir);

    static constexpr std::string_view error_string(ExtractError err) noexcept {
        switch (err) {
            using enum ExtractError;
            case OpenFileFailed:
                return "Failed to open TGZ file";
            case ReadFailed:
                return "Failed to read compressed data";
            case InvalidHeader:
                return "Invalid TAR header format";
            case InvalidChecksum:
                return "TAR header checksum validation failed";
            case CreateDirFailed:
                return "Failed to create directory";
            case WriteFileFailed:
                return "Failed to write output file";
            case PathTraversalDetected:
                return "Dangerous path detected (directory traversal attempt)";
            case FileTooLarge:
                return "File size exceeds maximum allowed size";
            case ArchiveTooLarge:
                return "Archive total size exceeds maximum allowed size";
            case SymlinkDetected:
                return "Symlink detected (potential security risk)";
            case UnicodeAttackDetected:
                return "Unicode-based path attack detected";
            case DiskFull:
                return "Disk full (insufficient space for extraction)";
        }
        std::unreachable();
    }
};

} // namespace archive
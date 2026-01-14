/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/tgz_extractor.hpp"
#include "include/file_descriptor.hpp"

#include <array>
#include <charconv>
#include <memory>
#include <span>
#include <vector>
#include <algorithm>
#include <numeric>
#include <optional>
#include <limits>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>

namespace calyx::core {

namespace {

static_assert(Config::TAR_BLOCK_SIZE == 512, "POSIX ustar block size must be 512 bytes");
static_assert(Config::TAR_PREFIX_OFFSET + Config::TAR_PREFIX_LENGTH <= Config::TAR_BLOCK_SIZE,
              "TAR header layout exceeds block size (Buffer Overflow Risk)");

std::uint64_t parse_octal(std::span<const std::byte> data) {
    const char* begin = reinterpret_cast<const char*>(data.data());
    const char* end = begin + data.size();

    while (begin < end && (*begin == ' ' || *begin == '\0'))
        begin++;

    const char* actual_end = begin;
    while (actual_end < end && *actual_end != '\0' && *actual_end != ' ')
        actual_end++;

    std::uint64_t value = 0;
    auto res = std::from_chars(begin, actual_end, value, 8);

    return (res.ec == std::errc{}) ? value : 0;
}

std::optional<std::string> get_safe_string(std::span<const std::byte> data) {
    const char* ptr = reinterpret_cast<const char*>(data.data());
    size_t len = 0;

    while (len < data.size() && ptr[len] != '\0') {
        unsigned char c = static_cast<unsigned char>(ptr[len]);

        if (c > 127 || (c < 32 && c != '\t')) {
            return std::nullopt;
        }

        len++;

        if (len > Config::TGZ_MAX_PATH_LENGTH) {
            return std::nullopt;
        }
    }

    if (len == 0) {
        return std::string{};
    }

    return std::string(ptr, len);
}

bool validate_checksum(std::span<const std::byte> header) {
    std::uint64_t calculated = 0;
    for (std::size_t i = 0; i < Config::TAR_BLOCK_SIZE; ++i) {
        if (i >= Config::TAR_CHECKSUM_OFFSET &&
            i < Config::TAR_CHECKSUM_OFFSET + Config::TAR_CHECKSUM_LENGTH) {
            calculated += static_cast<std::uint64_t>(' ');
        } else {
            calculated += static_cast<std::uint64_t>(header[i]);
        }
    }

    auto checksum_span = header.subspan(Config::TAR_CHECKSUM_OFFSET, Config::TAR_CHECKSUM_LENGTH);
    std::uint64_t stored = parse_octal(checksum_span);

    return calculated == stored;
}

std::expected<void, ExtractError> create_secure_directory(const std::filesystem::path& dir_path) {
    if (auto parent = dir_path.parent_path(); !parent.empty() && parent != dir_path) {
        if (auto result = create_secure_directory(parent); !result) {
            return result;
        }
    }

    if (::mkdir(dir_path.c_str(), S_IRWXU | S_IRGRP | S_IXGRP) == 0) {
        return {};
    }

    if (errno == EEXIST) {
        struct stat st;
        if (::lstat(dir_path.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                return {};
            }
            return std::unexpected(ExtractError::SymlinkDetected);
        }
    }

    return std::unexpected(ExtractError::CreateDirFailed);
}

bool is_safe_filename_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == ' ';
}

bool is_safe_filename(std::string_view filename) {
    if (filename.empty() || filename.length() > Config::TGZ_MAX_PATH_LENGTH) {
        return false;
    }

    if (filename == ".." || filename == "." || filename.starts_with(".") ||
        filename.ends_with(".") || filename.find("..") != std::string_view::npos) {
        return false;
    }

    for (char c : filename) {
        if (!is_safe_filename_char(c)) {
            return false;
        }
    }

    return true;
}

class SecureFileHandle {
   private:
    FileDescriptor fd_;
    std::filesystem::path file_path_;
    bool committed_ = false;

   public:
    explicit SecureFileHandle(const std::filesystem::path& path)
        : fd_(create_fd(path)), file_path_(path) {}

    SecureFileHandle(const SecureFileHandle&) = delete;
    SecureFileHandle& operator=(const SecureFileHandle&) = delete;

    bool write(const void* data, size_t size) {
        ssize_t written = ::write(fd_.get(), data, size);
        return written == static_cast<ssize_t>(size);
    }

    void commit() {
        committed_ = true;
    }

    ~SecureFileHandle() {
        if (!committed_ && std::filesystem::exists(file_path_)) {
            std::error_code ec;
            std::filesystem::remove(file_path_, ec);
        }
    }

    FileDescriptor& get_fd() {
        return fd_;
    }

   private:
    static int create_fd(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::remove(path, ec);

        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw std::system_error(
                ec.value(), std::generic_category(), "Failed to remove existing file");
        }

        int fd = ::open(
            path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);

        if (fd == -1) {
            if (errno == EEXIST || errno == ELOOP) {
                throw std::system_error(EEXIST,
                                        std::generic_category(),
                                        "Unable to create file: Target exists or is blocked");
            }
            throw std::system_error(errno, std::generic_category(), "Failed to create secure file");
        }

        return fd;
    }
};

std::optional<std::filesystem::path> sanitize_path(const std::filesystem::path& base_dir,
                                                   std::string_view path_str) {
    if (path_str.empty() || path_str.length() > Config::TGZ_MAX_TOTAL_PATH_LENGTH) {
        return std::nullopt;
    }

    for (char c : path_str) {
        unsigned char uc = static_cast<unsigned char>(c);

        if (uc > 127 || (uc < 32 && uc != '\t')) {
            return std::nullopt;
        }
    }

    if (path_str.find("../") != std::string_view::npos ||
        path_str.find("..\\") != std::string_view::npos ||
        path_str.find("//") != std::string_view::npos ||
        path_str.find("\\\\") != std::string_view::npos ||
        path_str.find(":\\") != std::string_view::npos || path_str.starts_with("/") ||
        path_str.starts_with("\\") || path_str.starts_with("~") || path_str.contains(";") ||
        path_str.contains("&") || path_str.contains("$") || path_str.contains("`") ||
        path_str.contains("|")) {
        return std::nullopt;
    }

    std::filesystem::path path(path_str);
    std::filesystem::path result = base_dir;

    std::uint32_t depth = 0;

    for (const auto& component : path) {
        if (++depth > Config::TGZ_MAX_PATH_DEPTH) {
            return std::nullopt;
        }

        std::string comp_str = component.string();

        if (!is_safe_filename(comp_str)) {
            return std::nullopt;
        }

        result /= component;
    }

    auto lexical_rel = std::filesystem::relative(result, base_dir);
    if (lexical_rel.empty() || lexical_rel.string().starts_with("..") ||
        lexical_rel.is_absolute()) {
        return std::nullopt;
    }

    return result;
}

struct GzFileDeleter {
    void operator()(gzFile file_handle) const {
        if (file_handle)
            gzclose(file_handle);
    }
};
using UniqueGzFile = std::unique_ptr<std::remove_pointer_t<gzFile>, GzFileDeleter>;

}  // namespace

std::string TgzExtractor::error_string(ExtractError err) {
    switch (err) {
        case ExtractError::OpenFileFailed:
            return "Failed to open TGZ file";
        case ExtractError::ReadFailed:
            return "Failed to read compressed data";
        case ExtractError::InvalidHeader:
            return "Invalid TAR header format";
        case ExtractError::InvalidChecksum:
            return "TAR header checksum validation failed";
        case ExtractError::CreateDirFailed:
            return "Failed to create directory";
        case ExtractError::WriteFileFailed:
            return "Failed to write output file";
        case ExtractError::PathTraversalDetected:
            return "Dangerous path detected (directory traversal attempt)";
        case ExtractError::FileTooLarge:
            return "File size exceeds maximum allowed size";
        case ExtractError::ArchiveTooLarge:
            return "Archive total size exceeds maximum allowed size";
        case ExtractError::SymlinkDetected:
            return "Symlink detected (potential security risk)";
        case ExtractError::UnicodeAttackDetected:
            return "Unicode-based path attack detected";
        default:
            return "Unknown error";
    }
}

std::expected<void, ExtractError> TgzExtractor::extract(const std::filesystem::path& tgz_path,
                                                        const std::filesystem::path& dest_dir) {
    UniqueGzFile gz(gzopen(tgz_path.c_str(), "rb"));
    if (!gz) {
        return std::unexpected(ExtractError::OpenFileFailed);
    }

    std::uint64_t total_extracted_size = 0;
    std::uint32_t file_count = 0;

    std::array<std::byte, Config::TAR_BLOCK_SIZE> header_block;

    while (true) {
        int bytes_read =
            gzread(gz.get(), header_block.data(), static_cast<unsigned>(header_block.size()));

        if (bytes_read < 0) {
            return std::unexpected(ExtractError::ReadFailed);
        }

        if (bytes_read == 0)
            break;

        if (bytes_read < static_cast<int>(Config::TAR_BLOCK_SIZE)) {
            return std::unexpected(ExtractError::InvalidHeader);
        }

        bool is_empty = std::all_of(header_block.begin(), header_block.end(), [](std::byte b) {
            return b == std::byte{0};
        });
        if (is_empty)
            break;

        if (++file_count > Config::TGZ_MAX_FILES) {
            return std::unexpected(ExtractError::ArchiveTooLarge);
        }

        if (!validate_checksum(std::span(header_block))) {
            return std::unexpected(ExtractError::InvalidChecksum);
        }

        std::span<const std::byte> block_span(header_block);

        auto name_result =
            get_safe_string(block_span.subspan(Config::TAR_NAME_OFFSET, Config::TAR_NAME_LENGTH));
        if (!name_result.has_value()) {
            return std::unexpected(ExtractError::InvalidHeader);
        }

        auto size_span = block_span.subspan(Config::TAR_SIZE_OFFSET, Config::TAR_SIZE_LENGTH);
        auto prefix_result = get_safe_string(
            block_span.subspan(Config::TAR_PREFIX_OFFSET, Config::TAR_PREFIX_LENGTH));
        if (!prefix_result.has_value()) {
            return std::unexpected(ExtractError::InvalidHeader);
        }

        std::string name_str = name_result.value();
        std::string prefix_str = prefix_result.value();
        char type_flag = static_cast<char>(header_block[Config::TAR_TYPE_OFFSET]);

        std::uint64_t file_size = parse_octal(size_span);

        if (type_flag == '1' || type_flag == '2') {
            return std::unexpected(ExtractError::SymlinkDetected);
        }

        if (file_size > Config::TGZ_MAX_FILE_SIZE) {
            return std::unexpected(ExtractError::FileTooLarge);
        }

        if (file_size > Config::TGZ_MAX_TOTAL_SIZE - total_extracted_size) {
            return std::unexpected(ExtractError::ArchiveTooLarge);
        }

        std::string full_path;
        if (!prefix_str.empty()) {
            full_path = prefix_str + "/" + name_str;
        } else {
            full_path = name_str;
        }

        auto safe_path = sanitize_path(dest_dir, full_path);
        if (!safe_path.has_value()) {
            return std::unexpected(ExtractError::PathTraversalDetected);
        }
        std::filesystem::path file_path = safe_path.value();

        if (type_flag == '5') {
            auto dir_result = create_secure_directory(file_path);
            if (!dir_result) {
                return std::unexpected(dir_result.error());
            }
        } else if (type_flag == '0' || type_flag == '\0') {
            if (file_path.has_parent_path()) {
                auto parent_result = create_secure_directory(file_path.parent_path());
                if (!parent_result) {
                    return std::unexpected(parent_result.error());
                }
            }

            try {
                SecureFileHandle secure_file(file_path);

                constexpr std::size_t BUFFER_SIZE = 16 * 1024;
                std::vector<std::byte> buffer(BUFFER_SIZE);
                std::uint64_t remaining = file_size;

                while (remaining > 0) {
                    std::size_t to_read = std::min<std::uint64_t>(remaining, buffer.size());
                    int chunk_read =
                        gzread(gz.get(), buffer.data(), static_cast<unsigned>(to_read));

                    if (chunk_read <= 0) {
                        return std::unexpected(ExtractError::ReadFailed);
                    }

                    if (!secure_file.write(buffer.data(), static_cast<std::size_t>(chunk_read))) {
                        return std::unexpected(ExtractError::WriteFileFailed);
                    }

                    remaining -= static_cast<std::uint64_t>(chunk_read);
                }

                std::size_t padding =
                    (Config::TAR_BLOCK_SIZE - (file_size % Config::TAR_BLOCK_SIZE)) %
                    Config::TAR_BLOCK_SIZE;
                if (padding > 0) {
                    if (gzseek(gz.get(), static_cast<z_off_t>(padding), SEEK_CUR) == -1) {
                        return std::unexpected(ExtractError::ReadFailed);
                    }
                }

                secure_file.commit();
                total_extracted_size += file_size;

            } catch (const std::system_error& e) {
                if (e.code().value() == EEXIST || e.code().value() == ELOOP) {
                    return std::unexpected(ExtractError::SymlinkDetected);
                }
                return std::unexpected(ExtractError::WriteFileFailed);
            }
        } else {
            std::uint64_t total_data = file_size;
            std::uint64_t full_blocks =
                (total_data + Config::TAR_BLOCK_SIZE - 1) / Config::TAR_BLOCK_SIZE;

            if (full_blocks > static_cast<std::uint64_t>(std::numeric_limits<z_off_t>::max()) /
                                  Config::TAR_BLOCK_SIZE) {
                return std::unexpected(ExtractError::FileTooLarge);
            }

            z_off_t skip_bytes = static_cast<z_off_t>(full_blocks * Config::TAR_BLOCK_SIZE);
            if (gzseek(gz.get(), skip_bytes, SEEK_CUR) == -1) {
                return std::unexpected(ExtractError::ReadFailed);
            }
            total_extracted_size += file_size;
        }
    }

    return {};
}

}  // namespace calyx::core
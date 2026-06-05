/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "tgz_extractor.hpp"

#include "config.hpp"
#include "file_descriptor.hpp"
#include "numeric_cast.hpp"
#include "random_engine.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <flat_set>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zlib.h>

namespace archive {

namespace {

/**
 * @brief Logic constants for TAR block alignment and layout validation.
 */
static_assert(config::kTarBlockSize == 512z, "POSIX ustar block size must be 512 bytes");
static_assert(config::kTarPrefixOffset + config::kTarPrefixLength <= config::kTarBlockSize,
    "TAR header layout exceeds block size (Buffer Overflow Risk)");

/**
 * @brief Parses numeric fields from TAR headers (Octal or Signed Base-256).
 *
 * @details Implements full GNU TAR signed base-256 compliance. Correctly detects
 * the sign bit (bit 6) of the first byte and returns nullopt for negative values
 * to prevent security risks associated with negative file sizes or modes.
 *
 * @param data A span of bytes containing the numeric field from the TAR header.
 * @return std::optional<std::uint64_t> The parsed value, or nullopt on overflow/malformed data.
 * @note Uses C++23 std::ranges::fold_left for safe, monadic overflow checking.
 */
std::optional<std::uint64_t> parse_numeric(std::span<const std::byte> data) {
    if (data.empty()) { return std::nullopt; }

    if ((std::to_integer<std::uint8_t>(data[0]) & 0x80) != 0) {
        const auto first_byte = std::to_integer<std::uint8_t>(data[0]);
        /**
         * @note Bit 6 is the sign bit in GNU TAR base-256.
         * Negative values are rejected for safety in size/mode fields.
         */
        if ((first_byte & 0x40) != 0) { return std::nullopt; }

        return std::ranges::fold_left(data | std::views::drop(1),
            std::optional<std::uint64_t> { toULong(first_byte & 0x3F) },
            [](auto accumulator, std::byte byte_value) -> std::optional<std::uint64_t> {
                return accumulator.and_then([byte_value](auto value) -> std::optional<std::uint64_t> {
                    if (value > (std::numeric_limits<std::uint64_t>::max() >> 8)) { return std::nullopt; }
                    return (value << 8) | std::to_integer<std::uint8_t>(byte_value);
                });
            });
    }

    const auto is_digit   = [](char ch) { return ch >= '0' && ch <= '7'; };
    const auto is_padding = [](char ch) { return ch == ' ' || ch == '\0'; };

    auto content = data | std::views::transform([](std::byte b) { return std::to_integer<char>(b); })
        | std::views::drop_while(is_padding);

    if (std::ranges::empty(content)) { return 0ULL; }

    auto octal_view = content | std::views::take_while(is_digit);
    if (std::ranges::empty(octal_view)) { return std::nullopt; }

    auto first_non_digit = content | std::views::drop_while(is_digit);
    if (!std::ranges::empty(first_non_digit) && !is_padding(*std::ranges::begin(first_non_digit))) {
        return std::nullopt;
    }

    return std::ranges::fold_left(
        octal_view | std::views::transform([](char character) { return toULong(character - '0'); }),
        std::optional<std::uint64_t> { 0ULL },
        [](auto accumulator, std::uint64_t digit) -> std::optional<std::uint64_t> {
            return accumulator.and_then([digit](auto value) -> std::optional<std::uint64_t> {
                if (value > (std::numeric_limits<std::uint64_t>::max() >> 3)) { return std::nullopt; }
                return (value << 3) | digit;
            });
        });
}

/**
 * @brief Safely extracts a null-terminated string from a fixed-length TAR field.
 *
 * @details Enforces maximum length constraints and filters out control characters
 * or non-ASCII data to prevent naming-based attacks (e.g., terminal escape sequences).
 *
 * @param data A span of bytes containing the string data.
 * @return std::optional<std::string> The sanitized string, or nullopt if invalid/unsafe.
 */
[[nodiscard]] std::optional<std::string> get_safe_string(std::span<const std::byte> data) {
    const auto null_iterator = std::ranges::find(data, std::byte { 0 });
    const auto total_length  = toSize(std::distance(data.begin(), null_iterator));

    if (total_length > config::kTgzMaxPathLength) { return std::nullopt; }

    const auto data_view = data.first(total_length);
    return std::ranges::any_of(data_view,
               [](std::byte byte_value) {
                   const auto unsigned_char = std::to_integer<unsigned char>(byte_value);
                   return unsigned_char >= 127 || (unsigned_char < 32 && unsigned_char != '\t');
               })
        ? std::nullopt
        : std::optional { std::ranges::to<std::string>(data_view
              | std::views::transform([](std::byte byte_value) { return std::to_integer<char>(byte_value); })) };
}

/**
 * @brief Validates the TAR block checksum.
 *
 * @details Supports both signed and unsigned checksum logic for compatibility
 * with various legacy tar implementations.
 *
 * @param header The 512-byte raw TAR header block.
 * @return bool True if the checksum matches either algorithm.
 */
[[nodiscard]] bool validate_checksum(std::span<const std::byte> header) {
    const auto [unsigned_sum, signed_sum]
        = std::ranges::fold_left(header | std::views::enumerate, std::pair<std::uint64_t, std::int64_t> {},
            [](auto checksum_pair, auto enum_pair) -> std::pair<std::uint64_t, std::int64_t> {
                auto [index, byte_val] = enum_pair;
                const auto byte_value  = (toSize(index) >= config::kTarChecksumOffset
                                            && toSize(index) < config::kTarChecksumOffset + config::kTarChecksumLength)
                     ? std::byte { ' ' }
                     : byte_val;
                return { checksum_pair.first + std::to_integer<std::uint64_t>(byte_value),
                    checksum_pair.second + toLong(std::to_integer<std::int8_t>(byte_value)) };
            });

    return parse_numeric(header.subspan(config::kTarChecksumOffset, config::kTarChecksumLength))
        .transform(
            [unsigned_sum, signed_sum](auto stored) { return stored == unsigned_sum || stored == toULong(signed_sum); })
        .value_or(false);
}

struct GzFileDeleter {
    void operator()(gzFile file_handle) const {
        if (file_handle) { gzclose(file_handle); }
    }
};
using UniqueGzFile = std::unique_ptr<std::remove_pointer_t<gzFile>, GzFileDeleter>;

/**
 * @brief RAII state for the extraction process.
 */
struct ExtractState {
    UniqueGzFile& gz; /**< Managed Gzip file handle */
    const std::filesystem::path& dest_dir; /**< Target extraction directory */
    std::flat_set<std::filesystem::path> validated_dirs; /**< Cache of verified sub-directories */
    std::uint64_t total_extracted_size { 0 }; /**< Cumulative size of extracted files */
    std::uint32_t file_count { 0 }; /**< Total number of files processed */
    std::optional<std::string> pending_long_path; /**< Stored path from GNU LongLink header ('L') */
    std::optional<std::string> pending_pax_path; /**< Stored path from PAX extended header ('x') */
    std::optional<std::uint64_t> pending_pax_size; /**< Stored size from PAX extended header ('x') */
    std::optional<std::string> global_pax_path; /**< Persistent path from global PAX header ('g') */
    std::optional<std::uint64_t> global_pax_size; /**< Persistent size from global PAX header ('g') */
};

inline constexpr std::size_t kTarMagicOffset     = 257z;
inline constexpr std::size_t kTarMagicLength     = 6z;
inline constexpr std::size_t kTarVersionOffset   = 263z;
inline constexpr std::size_t kTarVersionLength   = 2z;
inline constexpr std::size_t kTarMaxMetadataSize = 64z * 1024z;

/**
 * @brief Validates the 'ustar' magic and version fields.
 *
 * @param header The raw header block.
 * @return bool True if valid POSIX or GNU variant, or if the block is all zeros (termination).
 */
[[nodiscard]] bool validate_ustar_header(std::span<const std::byte> header) {
    const auto magic   = header.subspan(kTarMagicOffset, kTarMagicLength);
    const auto version = header.subspan(kTarVersionOffset, kTarVersionLength);

    if (std::ranges::all_of(magic, [](std::byte b) { return b == std::byte { 0 }; })) { return true; }

    const auto is_equal = [](std::span<const std::byte> span_data, std::string_view string_view_data) {
        return span_data.size() == string_view_data.size()
            && std::ranges::equal(span_data, string_view_data, {},
                [](std::byte byte_value) { return std::to_integer<char>(byte_value); });
    };

    return (is_equal(magic, { "ustar\0", 6 }) && is_equal(version, { "00", 2 }))
        || (is_equal(magic, { "ustar ", 6 }) && is_equal(version, { " \0", 2 }));
}

/**
 * @brief Recursively creates directories with strict symlink protection.
 *
 * @details Validates each segment of the path to ensure it is a real directory
 * and not a symlink, preventing "directory walking" attacks.
 *
 * @param dir_path The path to create.
 * @return std::expected<void, ExtractError> Success or the specific failure reason.
 * @warning Rejects the operation if any segment is a symlink.
 */
[[nodiscard]] std::expected<void, ExtractError> create_secure_directory(const std::filesystem::path& dir_path) {
    /**
     * @brief C++23 Recursive Lambda using Explicit Object Parameters (Deducing this).
     * @details Eliminates manual list building and reversal by using the call stack
     * to naturally walk the tree and create directories top-down during unwinding.
     */
    auto recurse = [](this auto self, const std::filesystem::path& p) -> std::expected<void, ExtractError> {
        if (p.empty() || p.parent_path() == p) { return {}; }

        if (auto res = self(p.parent_path()); !res) { return res; }

        auto mkdir_result = posix::mkdir(p, S_IRWXU | S_IRGRP | S_IXGRP);
        if (mkdir_result) { return {}; }
        if (mkdir_result.error() != std::errc::file_exists) { return std::unexpected(ExtractError::CreateDirFailed); }

        return posix::lstat(p)
            .transform_error([](auto) { return ExtractError::CreateDirFailed; })
            .and_then([](const struct stat& st) -> std::expected<void, ExtractError> {
                return S_ISDIR(st.st_mode) ? std::expected<void, ExtractError> {}
                                           : std::unexpected(ExtractError::SymlinkDetected);
            });
    };

    return recurse(dir_path);
}

[[nodiscard]] std::expected<void, ExtractError> create_secure_directory(
    const std::filesystem::path& dir_path, ExtractState& state) {
    auto normalized = dir_path.lexically_normal();
    if (normalized.empty()) { return {}; }

    /**
     * @brief Validate/create directory path and cache only after success.
     *
     * This overload intentionally does not short-circuit on cache membership.
     * It always executes the same security checks as create_secure_directory(path),
     * then records validated ancestor paths only after successful completion.
     */
    return create_secure_directory(normalized).transform([&state, &normalized] {
        std::filesystem::path current_path;
        std::ranges::for_each(normalized,
            [&state, &current_path](const auto& part) { state.validated_dirs.insert(current_path /= part); });
    });
}

[[nodiscard]] bool is_safe_filename(std::string_view filename) {
    if (filename.empty() || filename.length() > config::kTgzMaxPathLength) { return false; }

    if (filename == ".." || filename == ".") { return false; }

    return std::ranges::none_of(filename, [](char character) {
        const auto unsigned_char = toUChar(character);
        return unsigned_char >= 127 || (unsigned_char < 32 && character != '\t') || character == '/'
            || character == '\\';
    });
}

/**
 * @brief RAII handle for atomic file creation.
 *
 * @details Creates a temporary file during extraction and only renames it to
 * the final name upon successful commit(). Protects against partial extractions
 * and ensures that existing files are only replaced after full validation.
 */
class SecureFileHandle {
private:
    posix::file_descriptor fd_;
    std::filesystem::path temp_path_;
    std::filesystem::path final_path_;
    bool committed_ = false;

public:
    explicit SecureFileHandle(
        posix::file_descriptor&& descriptor, std::filesystem::path temp_path, std::filesystem::path final_path)
        : fd_(std::move(descriptor))
        , temp_path_(std::move(temp_path))
        , final_path_(std::move(final_path)) {}

    SecureFileHandle(const SecureFileHandle&)            = delete;
    SecureFileHandle& operator=(const SecureFileHandle&) = delete;

    SecureFileHandle(SecureFileHandle&& other) noexcept
        : fd_(std::move(other.fd_))
        , temp_path_(std::move(other.temp_path_))
        , final_path_(std::move(other.final_path_))
        , committed_(std::exchange(other.committed_, true)) {}

    SecureFileHandle& operator=(SecureFileHandle&& other) noexcept {
        if (this != &other) {
            if (!committed_ && !temp_path_.empty()) {
                std::error_code ec;
                std::filesystem::remove(temp_path_, ec);
            }
            fd_         = std::move(other.fd_);
            temp_path_  = std::move(other.temp_path_);
            final_path_ = std::move(other.final_path_);
            committed_  = std::exchange(other.committed_, true);
        }
        return *this;
    }

    [[nodiscard]] static std::expected<SecureFileHandle, std::error_code> create(const std::filesystem::path& path) {
        constexpr std::uint32_t kMaxTempCreateAttempts = 16;

        auto lstat_result = posix::lstat(path);
        if (lstat_result) {
            if (!S_ISREG(lstat_result->st_mode)) {
                return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
            }
        } else if (lstat_result.error() != std::errc::no_such_file_or_directory) {
            return std::unexpected(lstat_result.error());
        }

        const auto dir      = path.has_parent_path() ? path.parent_path() : std::filesystem::path { "." };
        const auto filename = path.filename().string();
        const auto pid      = posix::getpid();
        prng::Xoshiro256PlusPlus rng(toULong(std::random_device {}()));

        for (auto _ : std::views::iota(0u, kMaxTempCreateAttempts)) {
            auto temp_path = dir / std::format(".{}.tmp.{}.{:016x}", filename, pid, rng());
            auto opened    = posix::file::open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);

            if (opened) {
                return SecureFileHandle(opened->release(), std::move(temp_path), path);
            } else if (opened.error() != std::errc::file_exists) {
                return std::unexpected(opened.error());
            }
        }

        return std::unexpected(std::make_error_code(std::errc::file_exists));
    }

    [[nodiscard]] std::expected<void, std::error_code> write(std::span<const std::byte> data) {
        return fd_.write(data);
    }

    [[nodiscard]] std::expected<void, std::error_code> commit() {
        if (!fd_) [[unlikely]] { return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor)); }

        fd_.reset();

        std::error_code ec;
        std::filesystem::rename(temp_path_, final_path_, ec);
        if (ec) { return std::unexpected(ec); }

        committed_ = true;
        return {};
    }

    ~SecureFileHandle() {
        if (!committed_ && !temp_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(temp_path_, ec);
        }
    }

    posix::file_descriptor& get_fd() { return fd_; }
};

/**
 * @brief Sanity checks and normalizes a file path for secure extraction.
 *
 * @details Enforces:
 * - Anti-traversal (no '..')
 * - Relative paths only (relative to base_dir)
 * - Maximum path depth and total length
 * - Filename character safety
 *
 * @param base_dir The root directory for extraction.
 * @param path_str The raw path string from the archive.
 * @return std::optional<std::filesystem::path> The safe absolute path, or nullopt on hazard.
 */
std::optional<std::filesystem::path> sanitize_path(const std::filesystem::path& base_dir, std::string_view path_str) {
    if (path_str.empty() || path_str.length() > config::kTgzMaxTotalPathLength) { return std::nullopt; }

    if (std::ranges::any_of(path_str, [](char character) {
            const auto unsigned_char = toUChar(character);
            return unsigned_char >= 127 || (unsigned_char < 32 && character != '\t');
        })) {
        return std::nullopt;
    }

    const auto rel_path_res = [path_str]() -> std::optional<std::filesystem::path> {
        const auto normalized_path = std::filesystem::path(path_str).lexically_normal();
        if (normalized_path.empty() || normalized_path.is_absolute() || normalized_path.string().starts_with("..")) {
            return std::nullopt;
        }

        if (toSize(std::ranges::distance(normalized_path)) > config::kTgzMaxPathDepth) { return std::nullopt; }

        if (std::ranges::any_of(normalized_path, [](const auto& comp) {
                const auto name = comp.string();
                return !name.empty() && name != "." && !is_safe_filename(name);
            })) {
            return std::nullopt;
        }
        return normalized_path;
    }();

    if (!rel_path_res) { return std::nullopt; }
    return (base_dir / *rel_path_res).lexically_normal();
}

/**
 * @brief Categorizes TAR entry types to separate identification from execution logic.
 */
enum class EntryAction : std::uint8_t {
    Regular, /**< Extract as a regular file ('0', '\0') */
    Directory, /**< Create a directory ('5') */
    Skip, /**< Supported for metadata but data should be discarded ('x', 'g', 'L') or ignored (others) */
    Forbidden /**< Security risk: Symlinks and Hardlinks ('1', '2') */
};

/**
 * @brief O(1) lookup table for TAR type flags.
 */
static constexpr auto kTypeClassification = [] {
    std::array<EntryAction, 256> table {};
    table.fill(EntryAction::Skip);

    table[toSize('0')]  = EntryAction::Regular;
    table[toSize('\0')] = EntryAction::Regular;
    table[toSize('5')]  = EntryAction::Directory;
    table[toSize('1')]  = EntryAction::Forbidden;
    table[toSize('2')]  = EntryAction::Forbidden;

    table[toSize('x')] = EntryAction::Skip;
    table[toSize('g')] = EntryAction::Skip;
    table[toSize('L')] = EntryAction::Skip;
    table[toSize('K')] = EntryAction::Skip;

    return table;
}();

[[nodiscard]] std::expected<std::int32_t, ExtractError> gzread_checked(gzFile gz_handle, std::span<std::byte> buffer) {
    std::int32_t read_size = gzread(gz_handle, buffer.data(), toUInt(buffer.size()));
    if (read_size >= 0) { return read_size; }

    std::int32_t zlib_error            = Z_OK;
    [[maybe_unused]] auto zlib_message = gzerror(gz_handle, &zlib_error);

    switch (zlib_error) {
        case Z_DATA_ERROR:
        case Z_STREAM_ERROR:
        case Z_BUF_ERROR:
            return std::unexpected(ExtractError::InvalidHeader);
        default:
            return std::unexpected(ExtractError::ReadFailed);
    }
}

[[nodiscard]] std::expected<void, ExtractError> discard_bytes(gzFile gz, std::uint64_t total_bytes) {
    std::array<std::byte, config::kFileReadChunkSize> discard;
    for (std::uint64_t remaining = total_bytes; remaining > 0;) {
        const auto to_read = toSize(std::min<std::uint64_t>(remaining, discard.size()));
        const auto read    = gzread_checked(gz, std::span { discard }.first(to_read));
        if (!read || *read == 0) { return std::unexpected(ExtractError::ReadFailed); }
        remaining -= toSize(*read);
    }
    return {};
}

[[nodiscard]] std::expected<std::string, ExtractError> read_tar_payload(ExtractState& state, std::uint64_t size) {
    if (size > kTarMaxMetadataSize) { return std::unexpected(ExtractError::FileTooLarge); }

    std::string payload(toSize(size), '\0');
    for (std::uint64_t remaining = size, offset = 0; remaining > 0;) {
        const auto to_read = toSize(std::min<std::uint64_t>(remaining, config::kFileReadChunkSize));
        const auto read
            = gzread_checked(state.gz.get(), std::as_writable_bytes(std::span { payload }.subspan(offset, to_read)));
        if (!read || *read == 0) { return std::unexpected(ExtractError::ReadFailed); }

        const auto consumed = toSize(*read);
        remaining -= consumed;
        offset += consumed;
    }

    const auto padding = (config::kTarBlockSize - (size % config::kTarBlockSize)) % config::kTarBlockSize;
    return (padding > 0) ? discard_bytes(state.gz.get(), padding).transform([&payload] { return std::move(payload); })
                         : std::expected<std::string, ExtractError>(std::move(payload));
}

struct PaxRecordView {
    std::string_view key;
    std::string_view value;
    std::size_t total_len;
};

[[nodiscard]] std::expected<std::size_t, ExtractError> parse_record_length(std::string_view text) {
    std::size_t len = 0;
    auto [end, ec]  = std::from_chars(text.data(), text.data() + text.size(), len);
    if (ec != std::errc {} || end != text.data() + text.size()) { return std::unexpected(ExtractError::InvalidHeader); }
    return len;
}

[[nodiscard]] std::expected<PaxRecordView, ExtractError> parse_pax_record(std::string_view payload, std::size_t pos) {
    const auto space_pos = payload.find(' ', pos);
    if (space_pos == std::string_view::npos || space_pos == pos) {
        return std::unexpected(ExtractError::InvalidHeader);
    }

    return parse_record_length(payload.substr(pos, space_pos - pos))
        .and_then([payload, pos, space_pos](std::size_t record_len) -> std::expected<PaxRecordView, ExtractError> {
            const auto prefix_len = (space_pos - pos) + 1;
            const auto remaining  = payload.size() - pos;

            if (record_len <= prefix_len || record_len > remaining) {
                return std::unexpected(ExtractError::InvalidHeader);
            }

            auto content = payload.substr(space_pos + 1, record_len - prefix_len);
            if (content.empty() || content.back() != '\n') { return std::unexpected(ExtractError::InvalidHeader); }
            content.remove_suffix(1);

            auto eq = content.find('=');
            if (eq == std::string_view::npos) {
                return PaxRecordView { .key = {}, .value = {}, .total_len = record_len };
            }

            return PaxRecordView {
                .key = content.substr(0, eq), .value = content.substr(eq + 1), .total_len = record_len
            };
        });
}

[[nodiscard]] std::expected<std::uint64_t, ExtractError> parse_uint64(std::string_view text) {
    std::uint64_t val = 0;
    auto [end, ec]    = std::from_chars(text.data(), text.data() + text.size(), val);
    if (ec != std::errc {} || end != text.data() + text.size()) { return std::unexpected(ExtractError::InvalidHeader); }
    return val;
}

struct PaxMetadata {
    std::optional<std::string> path; /**< Extended path attribute */
    std::optional<std::uint64_t> size; /**< Extended size attribute */
};

[[nodiscard]] std::expected<PaxMetadata, ExtractError> parse_pax_metadata(std::string_view payload) {
    PaxMetadata metadata;

    for (std::size_t pos = 0; pos < payload.size();) {
        auto record = parse_pax_record(payload, pos);
        if (!record) { return std::unexpected(record.error()); }

        pos += record->total_len;
        if (record->key.empty()) { continue; }

        if (record->key == "path") {
            metadata.path = std::string(record->value);
        } else if (record->key == "size") {
            auto size = parse_uint64(record->value);
            if (!size) { return std::unexpected(size.error()); }
            metadata.size = *size;
        }
    }
    return metadata;
}

[[nodiscard]] std::expected<void, ExtractError> process_pax_entry(
    ExtractState& state, char type_flag, std::uint64_t file_size) {
    return read_tar_payload(state, file_size)
        .and_then(parse_pax_metadata)
        .transform([&state, type_flag](PaxMetadata metadata) {
            if (type_flag == 'x') {
                state.pending_pax_path = std::move(metadata.path);
                state.pending_pax_size = metadata.size;
                return;
            }

            if (metadata.path) { state.global_pax_path = std::move(metadata.path); }
            if (metadata.size) { state.global_pax_size = metadata.size; }
        });
}

[[nodiscard]] std::expected<void, ExtractError> skip_tar_data(ExtractState& state, std::uint64_t size) {
    if (size == 0) { return {}; }

    const auto padding = (config::kTarBlockSize - (size % config::kTarBlockSize)) % config::kTarBlockSize;
    return (size > std::numeric_limits<std::uint64_t>::max() - padding) ? std::unexpected(ExtractError::FileTooLarge)
                                                                        : discard_bytes(state.gz.get(), size + padding);
}

[[nodiscard]] std::expected<void, ExtractError> process_longlink_entry(ExtractState& state, std::uint64_t file_size) {
    return read_tar_payload(state, file_size)
        .and_then([&state](std::string raw_path) -> std::expected<void, ExtractError> {
            const auto path = [&raw_path]() -> std::string {
                auto sanitized_path = std::move(raw_path);
                if (auto nul = sanitized_path.find('\0'); nul != std::string::npos) { sanitized_path.resize(nul); }
                const auto last_valid = sanitized_path.find_last_not_of("\n\r");
                sanitized_path.resize(last_valid != std::string::npos ? last_valid + 1 : 0);
                return sanitized_path;
            }();

            if (path.empty()) { return std::unexpected(ExtractError::InvalidHeader); }
            state.pending_long_path = std::move(path);
            return {};
        });
}

[[nodiscard]] bool is_metadata_entry_type(char type_flag) {
    return type_flag == 'x' || type_flag == 'g' || type_flag == 'L' || type_flag == 'K';
}

[[nodiscard]] std::expected<void, ExtractError> process_metadata_entry(
    ExtractState& state, char type_flag, std::uint64_t file_size) {
    switch (type_flag) {
        case 'x':
        case 'g':
            return process_pax_entry(state, type_flag, file_size);
        case 'L':
            return process_longlink_entry(state, file_size);
        case 'K':
            return skip_tar_data(state, file_size);
        default:
            return std::unexpected(ExtractError::InvalidHeader);
    }
}

struct ParsedEntryHeader {
    std::uint64_t file_size;
    std::uint32_t file_mode;
    char type_flag;
};

[[nodiscard]] std::expected<ParsedEntryHeader, ExtractError> parse_entry_header(
    ExtractState& state, std::span<const std::byte> header_block) {
    if (!validate_checksum(header_block)) { return std::unexpected(ExtractError::InvalidChecksum); }
    if (!validate_ustar_header(header_block)) { return std::unexpected(ExtractError::InvalidHeader); }
    if (++state.file_count > config::kTgzMaxFiles) { return std::unexpected(ExtractError::ArchiveTooLarge); }

    const auto size_opt = parse_numeric(header_block.subspan(config::kTarSizeOffset, config::kTarSizeLength));
    const auto mode_opt = parse_numeric(header_block.subspan(config::kTarModeOffset, config::kTarModeLength));
    if (!size_opt || !mode_opt) { return std::unexpected(ExtractError::InvalidHeader); }

    return ParsedEntryHeader { .file_size = *size_opt,
        .file_mode                        = toUInt(*mode_opt),
        .type_flag                        = std::to_integer<char>(header_block[config::kTarTypeOffset]) };
}

[[nodiscard]] static ExtractError classify_file_creation_error(const std::error_code& ec) noexcept {
    if (posix::is_any_of(ec, std::errc::file_exists, std::errc::too_many_symbolic_link_levels,
            std::errc::operation_not_supported, std::errc::is_a_directory)) {
        return ExtractError::SymlinkDetected;
    }
    return ExtractError::WriteFileFailed;
}

[[nodiscard]] static ExtractError classify_commit_error(const std::error_code& ec) noexcept {
    if (posix::is_any_of(ec, std::errc::operation_not_supported, std::errc::is_a_directory,
            std::errc::too_many_symbolic_link_levels)) {
        return ExtractError::SymlinkDetected;
    }
    return ExtractError::WriteFileFailed;
}

[[nodiscard]] static ExtractError classify_write_error(const std::error_code& ec) noexcept {
    if (ec == std::errc::no_space_on_device || ec.value() == EDQUOT) { return ExtractError::DiskFull; }
    return ExtractError::WriteFileFailed;
}

[[nodiscard]] std::expected<void, ExtractError> copy_file_contents(
    ExtractState& state, SecureFileHandle& file, std::uint64_t size) {
    std::array<std::byte, config::kTgzDecompressionBufferSize> buf;
    for (std::uint64_t remaining = size; remaining > 0;) {
        const auto to_read = toSize(std::min<std::uint64_t>(remaining, buf.size()));
        auto read          = gzread_checked(state.gz.get(), std::span { buf }.first(to_read));
        if (!read || *read == 0) { return std::unexpected(ExtractError::ReadFailed); }

        auto write = file.write(std::span { buf }.first(toSize(*read)));
        if (!write) { return std::unexpected(classify_write_error(write.error())); }

        remaining -= toSize(*read);
    }
    return {};
}

[[nodiscard]] std::expected<void, ExtractError> skip_tar_padding(gzFile gz, std::uint64_t file_size) {
    const auto padding = (config::kTarBlockSize - (file_size % config::kTarBlockSize)) % config::kTarBlockSize;
    if (padding == 0) { return {}; }
    return discard_bytes(gz, padding);
}

[[nodiscard]] std::expected<void, ExtractError> apply_executable_mode(SecureFileHandle& file) {
    auto st = file.get_fd().stat();
    if (!st) { return std::unexpected(ExtractError::WriteFileFailed); }

    if (auto ch = file.get_fd().chmod((st->st_mode & mode_t { 07777 }) | S_IXUSR); !ch) {
        return std::unexpected(ExtractError::WriteFileFailed);
    }
    return {};
}

[[nodiscard]] std::expected<void, ExtractError> extract_regular_file(
    ExtractState& state, const std::filesystem::path& file_path, std::uint64_t file_size, std::uint32_t file_mode) {
    if (file_path.has_parent_path()) {
        if (auto res = create_secure_directory(file_path.parent_path(), state); !res) {
            return std::unexpected(res.error());
        }
    }

    auto file = SecureFileHandle::create(file_path);
    if (!file) { return std::unexpected(classify_file_creation_error(file.error())); }

    if (auto copy = copy_file_contents(state, *file, file_size); !copy) { return copy; }
    if (auto skip = skip_tar_padding(state.gz.get(), file_size); !skip) { return skip; }

    if (file_mode & 0100) {
        if (auto perm = apply_executable_mode(*file); !perm) { return perm; }
    }

    if (auto commit = file->commit(); !commit) { return std::unexpected(classify_commit_error(commit.error())); }
    return {};
}

struct EntryResolution {
    std::filesystem::path safe_path;
    std::uint64_t final_size;
};

struct EntryActionContext {
    std::filesystem::path safe_path;
    EntryAction action;
    std::uint64_t final_size;
    std::uint32_t file_mode;
};

[[nodiscard]] std::expected<void, ExtractError> validate_entry_limits(
    EntryAction action, std::uint64_t final_size, std::uint64_t total_extracted_size) {
    if (action == EntryAction::Forbidden) { return std::unexpected(ExtractError::SymlinkDetected); }
    if (final_size > config::kTgzMaxFileSize) { return std::unexpected(ExtractError::FileTooLarge); }
    if (final_size > config::kTgzMaxTotalSize - total_extracted_size) {
        return std::unexpected(ExtractError::ArchiveTooLarge);
    }
    return {};
}

[[nodiscard]] std::expected<EntryResolution, ExtractError> resolve_entry_path(
    ExtractState& state, std::span<const std::byte> header_block, std::uint64_t file_size) {
    const auto name_opt   = get_safe_string(header_block.subspan(config::kTarNameOffset, config::kTarNameLength));
    const auto prefix_opt = get_safe_string(header_block.subspan(config::kTarPrefixOffset, config::kTarPrefixLength));
    if (!name_opt || !prefix_opt) { return std::unexpected(ExtractError::InvalidHeader); }

    const auto full_path = [&state, &prefix_opt, &name_opt]() -> std::string {
        if (state.pending_pax_path) { return std::move(*state.pending_pax_path); }
        if (state.global_pax_path) { return *state.global_pax_path; }
        if (state.pending_long_path) { return std::move(*state.pending_long_path); }
        if (prefix_opt->empty()) { return std::move(*name_opt); }

        return std::format("{}/{}", *prefix_opt, *name_opt);
    }();

    const auto final_size    = state.pending_pax_size.value_or(state.global_pax_size.value_or(file_size));
    const auto safe_path_opt = sanitize_path(state.dest_dir, full_path);
    if (!safe_path_opt) { return std::unexpected(ExtractError::PathTraversalDetected); }

    return EntryResolution { .safe_path = std::move(*safe_path_opt), .final_size = final_size };
}

[[nodiscard]] std::expected<void, ExtractError> process_entry_action(
    ExtractState& state, const EntryActionContext& context) {
    const auto update_state = [&state, &context] { state.total_extracted_size += context.final_size; };

    switch (context.action) {
        case EntryAction::Directory:
            return create_secure_directory(context.safe_path, state)
                .and_then([&state, &context] { return skip_tar_data(state, context.final_size); })
                .transform(update_state);

        case EntryAction::Regular:
            if (auto res = check_disk_space(state.dest_dir, context.final_size); !res) {
                return std::unexpected(ExtractError::DiskFull);
            }
            return extract_regular_file(state, context.safe_path, context.final_size, context.file_mode)
                .transform(update_state);

        case EntryAction::Skip:
            return skip_tar_data(state, context.final_size).transform(update_state);

        case EntryAction::Forbidden:
            return std::unexpected(ExtractError::SymlinkDetected);
    }
    std::unreachable();
}

[[nodiscard]] std::expected<void, ExtractError> process_tar_entry(
    ExtractState& state, std::span<const std::byte> header_block) {
    return parse_entry_header(state, header_block)
        .and_then([&state, header_block](const ParsedEntryHeader& parsed_header) -> std::expected<void, ExtractError> {
            const auto file_size = parsed_header.file_size;
            const auto file_mode = parsed_header.file_mode;
            const auto type_flag = parsed_header.type_flag;

            /**
             * @brief Handle Metadata extensions (PAX, LongLink).
             * These headers provide extended attributes that override standard fields.
             */
            if (is_metadata_entry_type(type_flag)) { return process_metadata_entry(state, type_flag, file_size); }

            /**
             * @brief Resolve final path and size.
             * Priority: PAX > LongLink > Prefix + Name.
             *
             * @note  Non-metadata entries must reset pending state on exit to prevent
             *        stale data from incorrectly affecting subsequent entries if processing fails.
             */
            scope_exit reset_state { [&state]() noexcept {
                state.pending_pax_path.reset();
                state.pending_pax_size.reset();
                state.pending_long_path.reset();
            } };

            return resolve_entry_path(state, header_block, file_size)
                .and_then([&state, type_flag, file_mode](
                              const EntryResolution& resolution) -> std::expected<void, ExtractError> {
                    const auto action = kTypeClassification[toUChar(type_flag)];
                    return validate_entry_limits(action, resolution.final_size, state.total_extracted_size)
                        .and_then([&state, &resolution, action, file_mode] {
                            return process_entry_action(state,
                                EntryActionContext {
                                    .safe_path  = resolution.safe_path,
                                    .action     = action,
                                    .final_size = resolution.final_size,
                                    .file_mode  = file_mode,
                                });
                        });
                });
        });
}

} // namespace

/**
 * @brief Public API for TGZ extraction.
 *
 * @details Implements a hardened extraction loop that strictly enforces
 * security policies and resource limits.
 *
 * @param tgz_path Path to the source .tar.gz file.
 * @param dest_dir Destination directory for extraction.
 * @return std::expected<void, ExtractError> Result of the operation.
 */
std::expected<void, ExtractError> TgzExtractor::extract(
    const std::filesystem::path& tgz_path, const std::filesystem::path& dest_dir) {
    UniqueGzFile gz(gzopen(tgz_path.c_str(), "rb"));
    if (!gz) { return std::unexpected(ExtractError::OpenFileFailed); }

    ExtractState state { .gz  = gz,
        .dest_dir             = dest_dir,
        .validated_dirs       = {},
        .total_extracted_size = 0,
        .file_count           = 0,
        .pending_long_path    = std::nullopt,
        .pending_pax_path     = std::nullopt,
        .pending_pax_size     = std::nullopt,
        .global_pax_path      = std::nullopt,
        .global_pax_size      = std::nullopt };
    bool saw_first_zero_block = false;
    std::array<std::byte, config::kTarBlockSize> block_buffer {};

    while (true) {
        auto read_result = gzread_checked(state.gz.get(), block_buffer);
        if (!read_result) { return std::unexpected(read_result.error()); }

        const auto read_size = *read_result;
        if (read_size == 0 || toSize(read_size) < config::kTarBlockSize) {
            return std::unexpected(ExtractError::InvalidHeader);
        }

        const bool is_empty_block
            = std::ranges::all_of(block_buffer, [](std::byte byte_value) { return byte_value == std::byte { 0 }; });

        if (is_empty_block) {
            if (saw_first_zero_block) { break; }
            saw_first_zero_block = true;
            continue;
        }

        if (saw_first_zero_block) { return std::unexpected(ExtractError::InvalidHeader); }

        if (auto process_result = process_tar_entry(state, block_buffer); !process_result) {
            return std::unexpected(process_result.error());
        }
    }

    if (!gzeof(gz.get())) {
        std::int32_t zerr = Z_OK;
        gzerror(gz.get(), &zerr);
        if (zerr != Z_OK && zerr != Z_STREAM_END) { return std::unexpected(ExtractError::ReadFailed); }
    }

    return {};
}

} // namespace archive
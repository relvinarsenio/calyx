/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "file_descriptor.hpp"
#include "numeric_cast.hpp"
#include "scope.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <limits>
#include <linux/fs.h>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace posix {

/**
 * @brief Checks if a value matches any of the specified options.
 */
[[nodiscard]] constexpr bool is_any_of(const auto& value, const auto&... args) noexcept(
    (noexcept(value == args) && ...))
    requires(requires {
        { value == args } -> std::convertible_to<bool>;
    } && ...)
{
    return ((value == args) || ...);
}

/**
 * @brief Represents the alignment requirements for O_DIRECT I/O.
 *
 * Semantics match the Linux `statx(2)` DIOALIGN fields:
 * - @c mem_align corresponds to @c stx_dio_mem_align (buffer pointer alignment).
 * - @c offset_align corresponds to @c stx_dio_offset_align (file offset + I/O length alignment).
 *
 * Both default to 512 — the POSIX-mandated minimum sector size.
 */
struct BlockSize {
    std::uint32_t mem_align    = 512; ///< Buffer pointer alignment (stx_dio_mem_align).
    std::uint32_t offset_align = 512; ///< File offset + I/O length alignment (stx_dio_offset_align).
};

/**
 * @brief Type-safe wrapper around @c statx(2).
 *
 * Buries the raw @c ::statx syscall inside the abstraction layer,
 * matching the same pattern as @ref file_descriptor::ioctl.
 *
 * @param path   Path to the target file or directory.
 * @param mask   Bitmask of @c STATX_* flags indicating requested fields.
 * @param flags  One of @c AT_STATX_SYNC_AS_STAT (default), etc.
 * @return       Populated @c struct statx on success, or the captured errno.
 */
[[nodiscard]] inline auto statx(const std::filesystem::path& path, std::uint32_t mask,
    std::int32_t flags = AT_STATX_SYNC_AS_STAT) noexcept -> std::expected<struct ::statx, std::error_code> {
    struct ::statx stx {};
    return eintr_loop<error_style::posix>([&path, flags, mask, &stx]() noexcept {
        return ::statx(AT_FDCWD, path.c_str(), flags, mask, &stx);
    }).transform([&stx](auto) noexcept { return stx; });
}

namespace sys_helpers {

[[nodiscard]] inline auto block_size_from_statx([[maybe_unused]] const std::filesystem::path& path) noexcept
    -> std::optional<BlockSize> {
#if defined(STATX_DIOALIGN)
    if (auto stx = posix::statx(path, STATX_DIOALIGN);
        stx && (stx->stx_mask & STATX_DIOALIGN) && stx->stx_dio_mem_align > 0) {
        BlockSize sizes {};
        sizes.mem_align    = toUInt(stx->stx_dio_mem_align);
        sizes.offset_align = stx->stx_dio_offset_align > 0 ? toUInt(stx->stx_dio_offset_align) : sizes.mem_align;
        return sizes;
    }
#endif
    return std::nullopt;
}

[[nodiscard]] inline auto block_size_from_statfs(const std::filesystem::path& path) noexcept
    -> std::optional<BlockSize> {
    if (struct ::statfs sfs {}; ::statfs(path.c_str(), &sfs) == 0 && sfs.f_bsize > 0) {
        BlockSize sizes {};
        sizes.mem_align    = toUInt(sfs.f_bsize);
        sizes.offset_align = sizes.mem_align;
        return sizes;
    }
    return std::nullopt;
}

[[nodiscard]] inline auto block_size_from_ioctl(const std::filesystem::path& path) noexcept
    -> std::optional<BlockSize> {
    auto fd = file_descriptor::open(path, O_RDONLY);
    if (!fd) { return std::nullopt; }

    /** @brief BLKSSZGET: kernel uses put_int (expects int*). */
    std::int32_t logical_tmp = 0;
    if (!fd->ioctl(BLKSSZGET, logical_tmp) || logical_tmp <= 0) { return std::nullopt; }

    BlockSize sizes {};
    sizes.mem_align = toUInt(logical_tmp);

    /** @brief BLKPBSZGET: kernel uses put_uint (expects unsigned int*). */
    if (!fd->ioctl(BLKPBSZGET, sizes.offset_align)) { sizes.offset_align = sizes.mem_align; }

    return sizes;
}

[[nodiscard]] inline auto compute_poll_deadline(std::chrono::milliseconds timeout)
    -> std::pair<bool, std::chrono::steady_clock::time_point> {
    using clock         = std::chrono::steady_clock;
    const bool infinite = timeout.count() < 0;
    const auto deadline = infinite ? clock::time_point::max() : clock::now() + timeout;
    return { infinite, deadline };
}

[[nodiscard]] inline auto compute_poll_timeout_ms(
    bool infinite, std::chrono::steady_clock::time_point deadline) noexcept -> std::int32_t {
    using clock = std::chrono::steady_clock;
    if (infinite) { return -1; }

    const auto remaining = std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()),
        std::chrono::milliseconds { 0 }, std::chrono::milliseconds { std::numeric_limits<std::int32_t>::max() });
    return toInt(remaining.count());
}

[[nodiscard]] inline auto poll_deadline_reached(bool infinite, std::chrono::steady_clock::time_point deadline) noexcept
    -> bool {
    return !infinite && std::chrono::steady_clock::now() >= deadline;
}

} // namespace sys_helpers

/**
 * @brief Detect O_DIRECT alignment requirements for a filesystem path.
 *
 * Priority order:
 *  1. `posix::statx` with `STATX_DIOALIGN` (Linux \u2265 6.1) \u2014 reports the actual
 *     DIO memory and offset alignment required by the filesystem/block layer.
 *     Supported on block devices and regular files (ext4, f2fs, xfs).
 *  2. `statfs(2)` block size heuristic for generic filesystem paths.
 *  3. `BLKSSZGET` / `BLKPBSZGET` via @ref file_descriptor::ioctl \u2014 primarily for block devices.
 *  4. Hard-coded 512 B fallback.
 *
 * @param path  Path to the target file, directory, or block device.
 * @return      @c BlockSize with mem_align and offset_align values.
 */
[[nodiscard]] inline auto get_block_size(const std::filesystem::path& path) noexcept -> BlockSize {
    if (auto sizes = sys_helpers::block_size_from_statx(path)) { return *sizes; }
    if (auto sizes = sys_helpers::block_size_from_ioctl(path)) { return *sizes; }
    if (auto sizes = sys_helpers::block_size_from_statfs(path)) { return *sizes; }
    return BlockSize {};
}

/**
 * @brief Type-safe enumeration for POSIX file access advice (posix_fadvise).
 *
 * Replaces raw @c int @c POSIX_FADV_* constants; invalid values become
 * a compile-time error instead of silently misrouting the kernel hint.
 */
enum class FAdvise : std::int32_t {
    Normal     = POSIX_FADV_NORMAL,
    Sequential = POSIX_FADV_SEQUENTIAL,
    Random     = POSIX_FADV_RANDOM,
    NoReuse    = POSIX_FADV_NOREUSE,
    WillNeed   = POSIX_FADV_WILLNEED,
    DontNeed   = POSIX_FADV_DONTNEED,
};

/**
 * @brief Break the mutual dependency between @c File and its @c sys_helpers:: read helpers.
 *
 * @c File::read_all calls into @c sys_helpers::, but the helpers take @c File& —
 * so both sides need each other.  Forward-declaring @c File and the helper
 * signatures here lets the compiler resolve the cycle.
 */
class file;
namespace sys_helpers {
[[nodiscard]] auto read_regular_file(file& f, const struct ::stat& st) -> std::expected<std::string, std::error_code>;
[[nodiscard]] auto read_stream_file(file& f) -> std::expected<std::string, std::error_code>;
} // namespace sys_helpers

/**
 * @brief Type-safe RAII wrapper for POSIX file operations.
 *
 * Owns a @ref posix::file_descriptor and exposes read/write/sync operations
 * as methods returning @c std::expected.  All I/O handles @c EINTR
 * transparently.
 */
class file {
    posix::file_descriptor fd_;

    explicit file(posix::file_descriptor&& descriptor) noexcept
        : fd_(std::move(descriptor)) {}

public:
    file(file&&) noexcept            = default;
    file& operator=(file&&) noexcept = default;

    file(const file&)            = delete;
    file& operator=(const file&) = delete;

    /**
     * @brief Open a file with the given flags and permissions.
     *
     * @c O_CLOEXEC is always added to prevent fd leaks across @c exec.
     */
    [[nodiscard]] static auto open(const std::filesystem::path& path, std::int32_t flags, mode_t mode = 0)
        -> std::expected<file, std::error_code> {
        return posix::file_descriptor::open(path, flags, mode).transform([](posix::file_descriptor&& descriptor) {
            return file(std::move(descriptor));
        });
    }

    /**
     * @brief Open with @c O_DIRECT, falling back to buffered I/O if unsupported.
     *
     * Retries without @c O_DIRECT on @c EINVAL / @c EOPNOTSUPP / @c ENOTSUP.
     */
    [[nodiscard]] static auto open_direct(const std::filesystem::path& path, std::int32_t flags, mode_t mode = 0)
        -> std::expected<file, std::error_code> {
        auto result = eintr_loop<error_style::posix>(
            [&path, flags, mode]() noexcept { return ::open(path.c_str(), flags | O_DIRECT | O_CLOEXEC, mode); });

        if (!result && is_any_of(result.error().value(), EINVAL, EOPNOTSUPP, ENOTSUP)) {
            result = eintr_loop<error_style::posix>(
                [&path, flags, mode]() noexcept { return ::open(path.c_str(), flags | O_CLOEXEC, mode); });
        }

        return result.transform([](std::int32_t raw_fd) { return file(posix::file_descriptor { raw_fd }); });
    }

    /**
     * @brief Read entire file into a string.
     *
     * Pre-allocates for regular files; streams incrementally for
     * pseudo-files (@c /proc, @c /sys) that report @c st_size == 0.
     */
    [[nodiscard]] static auto read_all(const std::filesystem::path& path)
        -> std::expected<std::string, std::error_code> {
        auto file = open(path, O_RDONLY);
        if (!file) { return std::unexpected(file.error()); }

        auto file_stats = file->stat();
        if (!file_stats) { return std::unexpected(file_stats.error()); }

        if (file_stats->st_size > 0) { return sys_helpers::read_regular_file(*file, *file_stats); }
        return sys_helpers::read_stream_file(*file);
    }

    /**
     * @brief Create and write content to a file.
     *
     * Opens with @c O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW, permissions @c 0644.
     */
    [[nodiscard]] static auto write_to(const std::filesystem::path& path, std::string_view content)
        -> std::expected<void, std::error_code> {
        return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
            .and_then([content](file f) { return f.write(content); });
    }

    /** @brief Single read(2) with EINTR retry.  Zero return signals EOF. */
    [[nodiscard]] auto read(std::span<std::byte> buffer) const -> std::expected<std::size_t, std::error_code> {
        return fd_.read(buffer);
    }

    /** @brief Read until buffer is full or EOF. */
    [[nodiscard]] auto read_exact(std::span<std::byte> buffer) const -> std::expected<std::size_t, std::error_code> {
        return fd_.read_exact(buffer);
    }

    /** @brief Full-write loop with EINTR retry. */
    [[nodiscard]] auto write(std::span<const std::byte> data) const -> std::expected<void, std::error_code> {
        return fd_.write(data);
    }

    /** @brief Full-write loop with progress reporting on failure. */
    [[nodiscard]] auto write_exact(std::span<const std::byte> data) const
        -> std::expected<std::size_t, posix::file_descriptor::write_failure> {
        return fd_.write_exact(data);
    }

    /** @overload Convenience for string data. */
    [[nodiscard]] auto write(std::string_view data) const -> std::expected<void, std::error_code> {
        return write(std::as_bytes(std::span { data }));
    }

    /** @overload Convenience for string data with progress-aware contract. */
    [[nodiscard]] auto write_exact(std::string_view data) const
        -> std::expected<std::size_t, posix::file_descriptor::write_failure> {
        return write_exact(std::as_bytes(std::span { data }));
    }

    /** @brief File metadata via fstat(2). Delegates to file_descriptor::stat(). */
    [[nodiscard]] auto stat() const -> std::expected<struct ::stat, std::error_code> { return fd_.stat(); }

    /** @brief Flush data + metadata via fsync(2). Retries transparently on EINTR per POSIX. */
    [[nodiscard]] auto sync() const -> std::expected<void, std::error_code> { return fd_.sync(); }

    /** @brief Flush data only via fdatasync(2). Retries transparently on EINTR per POSIX. */
    [[nodiscard]] auto datasync() const -> std::expected<void, std::error_code> { return fd_.datasync(); }

    /**
     * @brief Advise kernel on access pattern via posix_fadvise(2).
     *
     * POSIX: returns 0 on success or an errno-compatible error code directly
     * (NOT the -1/errno pattern used by most syscalls).
     * EINTR is not listed in the POSIX error spec for this non-blocking call;
     * no retry loop needed.
     */
    [[nodiscard]] auto advise(off_t offset, off_t len, FAdvise advice) const -> std::expected<void, std::error_code> {
        return expect_success<error_style::pthreads>(
            ::posix_fadvise(fd_.native_handle(), offset, len, std::to_underlying(advice)));
    }

    /**
     * @brief Pre-allocate disk space via posix_fallocate(3).
     *
     * POSIX.1-2024: returns 0 on success or an errno-compatible error code
     * directly (NOT the -1/errno pattern).
     *
     * @note The abstract POSIX spec does not list EINTR, but the Linux
     *       fallocate(2) syscall can return it, and glibc/musl do NOT
     *       retry internally.  We add a retry loop for correctness.
     */
    [[nodiscard]] auto allocate(off_t offset, off_t len) const -> std::expected<void, std::error_code> {
        /**
         * @note posix_fallocate returns the error code directly (pthreads style), not -1/errno.
         * @note Linux fallocate(2) can return EINTR even though POSIX does not mandate it.
         */
        std::int32_t ec = 0;
        do {
            ec = ::posix_fallocate(fd_.native_handle(), offset, len);
        } while (ec == EINTR);
        return expect_success<error_style::pthreads>(ec);
    }

    /**
     * @brief Disable Copy-On-Write (CoW) on supported filesystems (e.g., BTRFS).
     *
     * Injects FS_NOCOW_FL via ioctl. Silently ignores errors if the underlying
     * filesystem does not support this flag. Must be called on an empty file
     * before any data or extents are allocated.
     */
    void disable_cow() const noexcept {
        const auto saved_errno { errno };
        scope_exit errno_guard { [saved_errno]() noexcept { errno = saved_errno; } };

        std::int32_t attr { 0 };
        if (::ioctl(fd_.native_handle(), toInt(FS_IOC_GETFLAGS), &attr) != 0) { return; }

        attr |= FS_NOCOW_FL;
        ::ioctl(fd_.native_handle(), toInt(FS_IOC_SETFLAGS), &attr);
    }

    /** @brief Access the underlying raw file descriptor. */
    [[nodiscard]] auto fd() const noexcept -> file_descriptor::native_handle_type { return fd_.native_handle(); }

    /** @brief Access the underlying posix::file_descriptor wrapper. */
    [[nodiscard]] const posix::file_descriptor& descriptor() const noexcept { return fd_; }

    /** @brief Relinquish ownership and return the underlying posix::file_descriptor. */
    [[nodiscard]] auto release() noexcept -> posix::file_descriptor { return std::move(fd_); }

    /** @brief Check whether the file holds a valid descriptor. */
    explicit operator bool() const noexcept { return static_cast<bool>(fd_); }
};

namespace sys_helpers {

/**
 * @brief Pre-allocated read for regular files with a known st_size.
 *
 * @c off_t is signed; validated before widening to @c std::size_t to
 * prevent negative/overflowed values from becoming large unsigned sizes.
 */
[[nodiscard]] inline auto read_regular_file(file& f, const struct ::stat& st)
    -> std::expected<std::string, std::error_code> {
    if (!std::in_range<std::size_t>(st.st_size)) {
        return std::unexpected(std::make_error_code(std::errc::file_too_large));
    }
    const std::size_t size = toSize(st.st_size);
    std::string content(size, '\0');
    auto bytes_read = f.read_exact(std::as_writable_bytes(std::span { content }));
    if (!bytes_read) { return std::unexpected(bytes_read.error()); }
    content.resize(*bytes_read);
    return content;
}

/**
 * @brief Streaming read for pseudo-files (@c /proc, @c /sys) where @c st_size == 0.
 *
 * Reads incrementally in chunks and caps at 64 MiB to prevent unbounded allocation.
 *
 * @note A short read (bytes_read < requested) is treated as EOF here.
 *       Per read(2), a short read does not strictly guarantee EOF, but
 *       procfs/sysfs generate content atomically — a partial return
 *       reliably indicates end-of-data for these pseudo-filesystems.
 */
[[nodiscard]] inline auto read_stream_file(file& f) -> std::expected<std::string, std::error_code> {
    static constexpr std::size_t kStreamReadMaxBytes  = 64uz * 1024uz * 1024uz;
    static constexpr std::size_t kStreamReadChunkSize = 16uz * 1024uz;
    std::string content {};
    std::size_t total_bytes = 0uz;

    while (true) {
        const auto remaining = kStreamReadMaxBytes - total_bytes;
        if (remaining == 0) { return std::unexpected(std::make_error_code(std::errc::file_too_large)); }

        const auto read_size = std::min<std::size_t>(kStreamReadChunkSize, remaining);
        content.resize(total_bytes + read_size);

        auto chunk      = std::as_writable_bytes(std::span { content }.subspan(total_bytes, read_size));
        auto bytes_read = f.read(chunk);

        if (!bytes_read) { return std::unexpected(bytes_read.error()); }
        if (*bytes_read == 0) { break; }

        total_bytes += *bytes_read;
        if (*bytes_read < read_size) { break; }
    }

    content.resize(total_bytes);
    return content;
}

} // namespace sys_helpers

/// ─── Free Functions: Filesystem & System Info ────────────────────────────────

/**
 * @brief Type-safe wrapper around mkdir(2).
 *
 * @param path Absolute or relative directory path to create.
 * @param mode Permission bits (default: @c 0755 = rwxr-xr-x).
 * @return     Success or the captured @c errno.
 */
[[nodiscard]] inline auto mkdir(const std::filesystem::path& path,
    mode_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) noexcept -> std::expected<void, std::error_code> {
    return eintr_loop<error_style::posix>([&path, mode]() noexcept {
        return ::mkdir(path.c_str(), mode);
    }).transform([](auto) noexcept {});
}

/**
 * @brief Type-safe wrapper around stat(2).
 *
 * @param path Path to the file or directory.
 * @return     Populated @c struct stat on success, or the captured @c errno.
 */
[[nodiscard]] inline auto stat(const std::filesystem::path& path) noexcept
    -> std::expected<struct ::stat, std::error_code> {
    struct ::stat st {};
    return expect_result<error_style::posix>(::stat(path.c_str(), &st)).transform([&st](auto) noexcept { return st; });
}

/**
 * @brief Type-safe wrapper around statvfs(3).
 *
 * @param path Path to any file within the mounted filesystem.
 * @return     Populated @c struct statvfs on success, or the captured @c errno.
 */
[[nodiscard]] inline auto statvfs(const std::filesystem::path& path) noexcept
    -> std::expected<struct ::statvfs, std::error_code> {
    struct ::statvfs svfs {};
    return expect_result<error_style::posix>(::statvfs(path.c_str(), &svfs)).transform([&svfs](auto) noexcept {
        return svfs;
    });
}

/**
 * @brief Type-safe wrapper around readlink(2).
 *
 * @param path Path to the symbolic link.
 * @param buf  Buffer to store the target path (should not be null-terminated by this).
 * @return     Number of bytes written, or the captured @c errno.
 */
[[nodiscard]] inline auto readlink(const std::filesystem::path& path, std::span<char> buf) noexcept
    -> std::expected<std::size_t, std::error_code> {
    /**
     * @note Per readlink(2), if the return value equals bufsiz,
     * truncation may have occurred. We conservatively treat this
     * as an error to avoid propagating ambiguous/truncated paths.
     */
    return expect_result<error_style::posix>(::readlink(path.c_str(), buf.data(), buf.size()))
        .and_then([&buf](ssize_t nbytes) -> std::expected<std::size_t, std::error_code> {
            if (toSize(nbytes) >= buf.size()) {
                return std::unexpected(std::make_error_code(std::errc::filename_too_long));
            }
            return toSize(nbytes);
        });
}

/**
 * @brief Type-safe wrapper around lstat(2).
 *
 * Returns metadata for the path itself (does not follow symlinks).
 * This is the correct function to use for symlink detection.
 *
 * @param path Path to inspect.
 * @return     Populated @c struct stat on success, or the captured @c errno.
 */
[[nodiscard]] inline auto lstat(const std::filesystem::path& path) noexcept
    -> std::expected<struct ::stat, std::error_code> {
    struct ::stat st {};
    return expect_result<error_style::posix>(::lstat(path.c_str(), &st)).transform([&st](auto) noexcept { return st; });
}

/**
 * @brief Type-safe wrapper around getpid(2).
 *
 * getpid(2) is async-signal-safe and always succeeds per POSIX.
 */
[[nodiscard]] inline auto getpid() noexcept -> pid_t {
    return ::getpid();
}

/**
 * @brief Type-safe wrapper around uname(2).
 *
 * @return Populated @c struct utsname on success, or the captured @c errno.
 */
[[nodiscard]] inline auto uname() noexcept -> std::expected<struct ::utsname, std::error_code> {
    struct ::utsname uts {};
    return expect_success<error_style::posix>(::uname(&uts)).transform([&uts]() { return uts; });
}

/**
 * @brief Type-safe wrapper around getrlimit(2).
 *
 * @param resource The resource type (e.g., RLIMIT_MEMLOCK).
 * @return         Populated @c struct rlimit on success, or the captured @c errno.
 */
[[nodiscard]] inline auto get_rlimit(std::int32_t resource) noexcept
    -> std::expected<struct ::rlimit, std::error_code> {
    struct ::rlimit limit {};
    return expect_success<error_style::posix>(::getrlimit(resource, &limit)).transform([&limit]() { return limit; });
}

/**
 * @brief Resolves an executable's path using the PATH environment variable.
 *
 * MUST be called before fork() in multi-threaded programs to avoid invoking
 * async-signal-unsafe functions (like getenv, malloc, and implicit path
 * searches found in execvp) within the post-fork child.
 *
 * @param cmd The command to resolve (e.g., "tar").
 * @return Resolved path + opened executable fd, or ENOENT if not found.
 */
struct resolved_executable {
    std::string path {};
    posix::file_descriptor fd {};
};

namespace sys_helpers {

[[nodiscard]] inline auto verify_execute_access(const posix::file_descriptor& opened_fd,
    const std::filesystem::path& path) noexcept -> std::expected<void, std::error_code> {
#if defined(AT_EMPTY_PATH)
    if (auto first = expect_success<error_style::posix>(
            ::faccessat(opened_fd.native_handle(), "", X_OK, AT_EMPTY_PATH | AT_EACCESS));
        !first) {
        const std::int32_t saved_errno = first.error().value();
        if (saved_errno != EINVAL && saved_errno != ENOSYS) { return std::unexpected(first.error()); }
        /**
         * @brief Fallback for kernels/libcs without full AT_EMPTY_PATH support.
         *
         * @details Keep AT_EACCESS semantics (effective IDs), matching the
         * main path above and avoiding behavior drift to real-ID checks.
         */
        return eintr_loop<error_style::posix>([&path]() noexcept {
            return ::faccessat(AT_FDCWD, path.c_str(), X_OK, AT_EACCESS);
        }).transform([](auto) noexcept {});
    }
#else
    return eintr_loop<error_style::posix>([&path]() noexcept {
        return ::access(path.c_str(), X_OK);
    }).transform([](auto) noexcept {});
#endif
    return {};
}

[[nodiscard]] inline auto duplicate_safe_exec_fd(posix::file_descriptor& opened_fd) noexcept
    -> std::expected<posix::file_descriptor, std::error_code> {
    /**
     * @brief Ensure the executable descriptor never aliases stdin/stdout/stderr.
     *
     * @details `ShellPipe` child redirects `STDOUT_FILENO` and `STDERR_FILENO`
     * before calling `exec_fd()`. If the resolved executable descriptor were
     * `1` or `2`, redirection could replace the executable handle and break
     * `fexecve`.
     */
    const auto fd_flags = eintr_loop<error_style::posix>(
        [&opened_fd]() noexcept { return ::fcntl(opened_fd.native_handle(), F_GETFD); });
    if (!fd_flags) { return std::unexpected(fd_flags.error()); }

    if (opened_fd.native_handle() > posix::file_descriptor::stderr_fd && (*fd_flags & FD_CLOEXEC) != 0) {
        return std::move(opened_fd);
    }

    auto safe_fd = eintr_loop<error_style::posix>([&opened_fd]() noexcept {
        return ::fcntl(opened_fd.native_handle(), F_DUPFD_CLOEXEC, posix::file_descriptor::stderr_fd + 1);
    });
    if (!safe_fd) { return std::unexpected(safe_fd.error()); }

    opened_fd.reset();
    return posix::file_descriptor { *safe_fd };
}

[[nodiscard]] inline auto open_executable_file(const std::filesystem::path& path) noexcept
    -> std::expected<posix::file_descriptor, std::error_code> {
    auto opened = posix::file_descriptor::open(path, O_RDONLY);
    if (!opened) { return std::unexpected(opened.error()); }

    auto st = opened->stat();
    if (!st) { return std::unexpected(st.error()); }
    if (!S_ISREG(st->st_mode)) { return std::unexpected(std::make_error_code(std::errc::permission_denied)); }

    if (auto access = verify_execute_access(*opened, path); !access) { return std::unexpected(access.error()); }

    return duplicate_safe_exec_fd(*opened);
}

[[nodiscard]] inline auto read_path_environment(std::string& fallback_path)
    -> std::expected<std::string_view, std::error_code> {
    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        const auto size = ::confstr(_CS_PATH, nullptr, 0);
        if (size == 0) { return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory)); }
        fallback_path.resize(size - 1);
        [[maybe_unused]] const auto written = ::confstr(_CS_PATH, fallback_path.data(), size);
        path_env                            = fallback_path.c_str();
    }

    return std::string_view { path_env };
}

[[nodiscard]] inline auto resolve_from_path(std::string_view paths, std::string_view cmd)
    -> std::expected<resolved_executable, std::error_code> {
    std::error_code last_err = std::make_error_code(std::errc::no_such_file_or_directory);

    for (auto part : paths | std::views::split(':')) {
        const std::string_view dir { part };
        std::filesystem::path path { dir.empty() ? "." : dir };
        path /= cmd;

        auto opened = open_executable_file(path);
        if (opened) { return resolved_executable { std::move(path).string(), std::move(*opened) }; }
        if (opened.error() != std::errc::no_such_file_or_directory) { last_err = opened.error(); }
    }

    return std::unexpected(last_err);
}

} // namespace sys_helpers

[[nodiscard]] inline auto resolve_executable(std::string_view cmd)
    -> std::expected<resolved_executable, std::error_code> {
    if (cmd.contains('/')) {
        auto opened = sys_helpers::open_executable_file(cmd);
        if (!opened) { return std::unexpected(opened.error()); }
        return resolved_executable { std::string(cmd), std::move(*opened) };
    }

    std::string fallback_path {};
    auto path_env = sys_helpers::read_path_environment(fallback_path);
    if (!path_env) { return std::unexpected(path_env.error()); }

    return sys_helpers::resolve_from_path(*path_env, cmd);
}

/// ─── Pipe: RAII wrapper for pipe2(2) ─────────────────────────────────────────
/// Per pipe2(2): "pipefd[0] refers to the read end of the pipe.
///                pipefd[1] refers to the write end of the pipe."
/// O_CLOEXEC is set atomically to prevent fd leak across fork+exec.

/**
 * @brief RAII wrapper for a POSIX pipe created via pipe2(2).
 *
 * Holds both ends as @ref file_descriptor instances.  Created exclusively
 * through @ref pipe::create() which atomically sets @c O_CLOEXEC.
 *
 * @see pipe2(2) — On success, zero is returned. On error, -1 is returned,
 *      errno is set, and pipefd is left unchanged.
 */
class pipe {
    file_descriptor read_end_;
    file_descriptor write_end_;

    pipe(file_descriptor&& r, file_descriptor&& w) noexcept
        : read_end_(std::move(r))
        , write_end_(std::move(w)) {}

public:
    pipe(pipe&&) noexcept            = default;
    pipe& operator=(pipe&&) noexcept = default;

    pipe(const pipe&)            = delete;
    pipe& operator=(const pipe&) = delete;

    /**
     * @brief Create a pipe with @c O_CLOEXEC set atomically.
     *
     * Uses pipe2(2) which is POSIX.1-2024 and available since Linux 2.6.27.
     * O_CLOEXEC prevents the file descriptors from leaking into child
     * processes spawned via fork+exec.
     *
     * @return Pipe on success, or the captured @c errno.
     */
    [[nodiscard]] static auto create() noexcept -> std::expected<pipe, std::error_code> {
        std::array<file_descriptor::native_handle_type, 2> fds {};
        return eintr_loop<error_style::posix>([&fds]() noexcept {
            return ::pipe2(fds.data(), O_CLOEXEC);
        }).transform([&fds](auto) noexcept { return pipe(file_descriptor(fds[0]), file_descriptor(fds[1])); });
    }

    /** @brief Access the read end of the pipe (pipefd[0]). */
    [[nodiscard]] auto& read_end(this auto& self) noexcept { return self.read_end_; }

    /** @brief Access the write end of the pipe (pipefd[1]). */
    [[nodiscard]] auto& write_end(this auto& self) noexcept { return self.write_end_; }

    /** @brief Release ownership of the read end. */
    [[nodiscard]] auto release_read() noexcept -> file_descriptor { return std::move(read_end_); }

    /** @brief Release ownership of the write end. */
    [[nodiscard]] auto release_write() noexcept -> file_descriptor { return std::move(write_end_); }
};

/// ─── wait_status: type-safe wrapper for wait(2) status macros ─────────────────
/// Per waitpid(2): "WIFEXITED returns true if the child terminated normally."
///   WEXITSTATUS: "least significant 8 bits of the status argument".
///   WIFSIGNALED: "returns true if the child process was terminated by a signal".
///   WTERMSIG:    "returns the number of the signal that caused termination".
/// These macros MUST only be used when the corresponding WIF* predicate is true.

/**
 * @brief Type-safe wrapper around the raw wait(2) status integer.
 *
 * Encapsulates the @c WIFEXITED / @c WEXITSTATUS / @c WIFSIGNALED / @c WTERMSIG
 * macros behind a clean C++23 interface.  Methods that extract data (exit_code,
 * term_signal) are only valid when the corresponding predicate (exited,
 * signaled) is true, per POSIX spec.
 */
class wait_status {
    std::int32_t raw_;
    pid_t pid_;

public:
    explicit wait_status(std::int32_t raw_status, pid_t waited_pid) noexcept
        : raw_(raw_status)
        , pid_(waited_pid) {}

    /** @brief PID of the process this status belongs to. */
    [[nodiscard]] auto pid() const noexcept -> pid_t { return pid_; }

    /** @brief True if the process exited normally (via exit(3) or _exit(2)). */
    [[nodiscard]] auto exited() const noexcept -> bool { return WIFEXITED(raw_); }

    /**
     * @brief Exit code if @ref exited() is true.
     * @pre   exited() must be true per POSIX — undefined behavior otherwise.
     */
    [[nodiscard]] auto exit_code() const noexcept -> std::int32_t { return WEXITSTATUS(raw_); }

    /** @brief True if the process was terminated by a signal. */
    [[nodiscard]] auto signaled() const noexcept -> bool { return WIFSIGNALED(raw_); }

    /**
     * @brief Signal number if @ref signaled() is true.
     * @pre   signaled() must be true per POSIX — undefined behavior otherwise.
     */
    [[nodiscard]] auto term_signal() const noexcept -> std::int32_t { return WTERMSIG(raw_); }

    /**
     * @brief Describe the termination signal as a human-readable string.
     *
     * Uses strsignal(3), which is NOT thread-safe per POSIX.1-2024.
     * On musl (static builds), strsignal returns a pointer to a static
     * string table — effectively thread-safe.  The result is copied
     * into std::string immediately to minimise the race window.
     */
    [[nodiscard]] auto describe_signal() const -> std::string {
        if (!signaled()) { return {}; }
        const std::int32_t sig = term_signal();
        const char* msg        = ::strsignal(sig);
        return msg ? std::format("{} ({})", msg, sig) : std::format("Unknown Signal ({})", sig);
    }

    /** @brief Access the raw wait status bits for interop. */
    [[nodiscard]] auto raw() const noexcept -> std::int32_t { return raw_; }
};

/// ─── Process Lifecycle ───────────────────────────────────────────────────────

/**
 * @brief Type-safe wrapper around fork(2).
 *
 * Per fork(2): "On success, the PID of the child process is returned in
 * the parent, and 0 is returned in the child. On failure, -1 is returned
 * in the parent, no child process is created, and errno is set."
 *
 * Notable error codes per man page:
 * - EAGAIN: system thread/process limit reached
 * - ENOMEM: insufficient kernel memory
 *
 * @return 0 in the child process, the child PID in the parent, or @c errno.
 */
[[nodiscard]] inline auto fork() noexcept -> std::expected<pid_t, std::error_code> {
    return expect_result<error_style::posix>(::fork());
}

/**
 * @brief Type-safe wrapper around execv(3).
 *
 * Replaces the current process image.  Only returns on failure (sets errno).
 * The caller is responsible for calling @c ::_exit() after a failed exec in a
 * post-fork child — using async-signal-safe functions only.
 *
 * @param path  Resolved absolute or relative path to the executable.
 * @param argv  Null-terminated argument array (argv[0] = command name).
 * @note  In a post-fork child, only async-signal-safe functions are allowed
 *        per POSIX signal-safety(7).  This wrapper is intentionally minimal.
 */
inline void exec(const char* path, char* const argv[]) noexcept {
    ::execv(path, argv);
    /// Only reached on failure — caller must _exit().
}

/**
 * @brief Execute a previously opened executable file descriptor.
 *
 * Uses @c fexecve to avoid path TOCTOU between resolution and execution.
 * If @c fexecve fails with @c ENOENT (known script + FD_CLOEXEC case on Linux),
 * falls back to @c execv(path, argv) to preserve script compatibility.
 *
 * @param fd    Opened executable descriptor.
 * @param path  Original resolved path for fallback.
 * @param argv  Null-terminated argument array.
 */
inline void exec_fd(file_descriptor::native_handle_type fd, const char* path, char* const argv[]) noexcept {
    ::fexecve(fd, argv, ::environ);

    if (errno == ENOENT) { ::execv(path, argv); }
}

/**
 * @brief Type-safe wrapper around waitpid(2) with automatic EINTR retry.
 *
 * Per waitpid(2):
 * - Returns child PID on success, 0 if WNOHANG and no state change, -1 on error.
 * - EINTR: "WNOHANG was not set and an unblocked signal or SIGCHLD was caught."
 *   We transparently retry on EINTR per standard POSIX practice.
 * - ECHILD: "The process specified by pid does not exist or is not a child."
 *
 * @param pid     PID to wait for, or -1 for any child.
 * @param options @c WNOHANG, @c WUNTRACED, etc.
 * @return        @ref wait_status on success, or the captured @c errno.
 */
[[nodiscard]] inline auto waitpid(pid_t pid, std::int32_t options = 0) noexcept
    -> std::expected<std::optional<wait_status>, std::error_code> {
    std::int32_t status = 0;
    return eintr_loop<error_style::posix>([pid, options, &status]() noexcept {
        return ::waitpid(pid, &status, options);
    }).and_then([&status](pid_t child_pid) -> std::expected<std::optional<wait_status>, std::error_code> {
        if (child_pid == 0) { return std::nullopt; }
        return wait_status(status, child_pid);
    });
}

/**
 * @brief Strongly-typed POSIX signals.
 */
enum class signal : std::int32_t {
    Term = SIGTERM,
    Kill = SIGKILL,
    Int  = SIGINT,
    Quit = SIGQUIT,
    Hup  = SIGHUP,
    Pipe = SIGPIPE,
    Chld = SIGCHLD,
    Usr1 = SIGUSR1,
    Usr2 = SIGUSR2
};

/**
 * @brief Type-safe wrapper around kill(2).
 *
 * Per kill(2): "On success, zero is returned. On error, -1 is returned,
 * and errno is set."
 *
 * Notable error codes:
 * - EINVAL: invalid signal
 * - EPERM:  no permission to send signal
 * - ESRCH:  process/group does not exist (may be zombie)
 *
 * @param pid    Target process ID (positive), process group (negative), etc.
 * @param sig    Signal enum (e.g. @c posix::signal::Term, @c posix::signal::Kill).
 * @return       Success or the captured @c errno.
 */
[[nodiscard]] inline auto kill(pid_t pid, signal sig) noexcept -> std::expected<void, std::error_code> {
    return expect_success<error_style::posix>(::kill(pid, std::to_underlying(sig)));
}

/// ─── I/O Multiplexing ───────────────────────────────────────────────────────

/**
 * @brief Type-safe wrapper around poll(2) with @c std::chrono timeout.
 *
 * Per poll(2):
 * - Returns nonnegative count of ready fds on success.
 * - Returns 0 on timeout (no fd became ready).
 * - Returns -1 on error with errno set.
 * - EINTR: "A signal occurred before any requested event" — we retry
 *   transparently and adjust the remaining deadline to prevent spurious
 *   wakeups from silently consuming the timeout.
 *
 * @param fds     Span of @c pollfd entries to monitor.
 * @param timeout Maximum time to wait.  Use @c milliseconds::zero() for
 *                non-blocking poll, or @c milliseconds(-1) for infinite wait.
 * @return        Number of ready descriptors, or the captured @c errno.
 */
[[nodiscard]] inline auto poll(std::span<::pollfd> fds, std::chrono::milliseconds timeout) noexcept
    -> std::expected<std::int32_t, std::error_code> {
    auto [infinite, deadline] = sys_helpers::compute_poll_deadline(timeout);

    while (true) {
        const std::int32_t timeout_ms = sys_helpers::compute_poll_timeout_ms(infinite, deadline);

        const std::int32_t res = ::poll(fds.data(), toUInt(fds.size()), timeout_ms);

        if (auto result = expect_result<error_style::posix>(res); result || result.error() != std::errc::interrupted) {
            return result;
        }
        /// EINTR received — check if deadline has passed before retrying
        if (sys_helpers::poll_deadline_reached(infinite, deadline)) {
            return 0; ///< Treat as timeout
        }
    }
}

/**
 * @brief Create a unique temporary directory via mkdtemp(3).
 *
 * @param tmpl  Path template ending in "XXXXXX" (modified in-place on success).
 * @return      The created directory path, or @c errno on failure.
 */
[[nodiscard]] inline auto make_temp_dir(std::string tmpl) noexcept -> std::expected<std::string, std::error_code> {
    if (::mkdtemp(tmpl.data())) { return tmpl; }
    return std::unexpected(last_error());
}

/**
 * @brief Concept constraining valid POSIX socket address structures (POSIX.1, RFC 3493).
 */
template <typename T>
concept socket_address = std::is_standard_layout_v<std::remove_cvref_t<T>>
    && std::is_trivially_copyable_v<std::remove_cvref_t<T>> && requires(const std::remove_cvref_t<T>& sa) {
           requires requires {
               { sa.sin_family } -> std::convertible_to<sa_family_t>;
           } || requires {
               { sa.sin6_family } -> std::convertible_to<sa_family_t>;
           } || requires {
               { sa.sa_family } -> std::convertible_to<sa_family_t>;
           };
       };

/**
 * @brief Concept constraining valid binary network address structures.
 */
template <typename T>
concept network_address = std::is_standard_layout_v<std::remove_cvref_t<T>>
    && std::is_trivially_copyable_v<std::remove_cvref_t<T>> && requires(const std::remove_cvref_t<T>& addr) {
           requires requires {
               { addr.s_addr } -> std::convertible_to<in_addr_t>;
           } || requires {
               { addr.s6_addr };
           } || std::is_same_v<std::remove_cvref_t<T>, struct in_addr> || std::is_same_v<std::remove_cvref_t<T>, struct in6_addr>;
       };

/**
 * @brief Convert socket address structure reference to const sockaddr pointer.
 */
template <socket_address Addr> [[nodiscard]] inline auto to_sockaddr(const Addr& addr) noexcept -> const sockaddr* {
    return std::bit_cast<const sockaddr*>(std::addressof(addr));
}

/**
 * @brief Convert socket address structure reference to mutable sockaddr pointer.
 */
template <socket_address Addr> [[nodiscard]] inline auto to_sockaddr(Addr& addr) noexcept -> sockaddr* {
    return std::bit_cast<sockaddr*>(std::addressof(addr));
}

/**
 * @brief Extract address family from a POSIX socket address pointer.
 */
[[nodiscard]] inline auto get_address_family(const sockaddr* sa) noexcept -> sa_family_t {
    return sa != nullptr ? sa->sa_family : AF_UNSPEC;
}

/**
 * @brief Extract address family from any socket address structure.
 */
template <socket_address Addr> [[nodiscard]] inline auto get_address_family(const Addr& addr) noexcept -> sa_family_t {
    return get_address_family(to_sockaddr(addr));
}

/**
 * @brief Create communication endpoint.
 */
[[nodiscard]] inline auto socket(std::int32_t domain, std::int32_t type, std::int32_t protocol) noexcept
    -> std::expected<file_descriptor, std::error_code> {
    return expect_result<error_style::posix>(::socket(domain, type, protocol))
        .transform([](std::int32_t raw_fd) noexcept { return file_descriptor { raw_fd }; });
}

/**
 * @brief Initiate connection on socket.
 */
template <socket_address Addr>
[[nodiscard]] inline auto connect(const file_descriptor& fd, const Addr& addr) noexcept
    -> std::expected<void, std::error_code> {
    const auto len { cast::static_converter<socklen_t> {}(sizeof(addr)) };
    return expect_result<error_style::posix>(::connect(fd.native_handle(), to_sockaddr(addr), len))
        .transform([](auto) noexcept {});
}

/**
 * @brief Send message on socket.
 */
template <socket_address Addr>
[[nodiscard]] inline auto sendto(const file_descriptor& fd, std::span<const std::byte> data, const Addr& dest_addr,
    std::int32_t flags = 0) noexcept -> std::expected<std::size_t, std::error_code> {
    const auto len { cast::static_converter<socklen_t> {}(sizeof(dest_addr)) };
    return expect_result<error_style::posix>(
        ::sendto(fd.native_handle(), data.data(), data.size(), flags, to_sockaddr(dest_addr), len))
        .transform([](auto bytes) noexcept { return toSize(bytes); });
}

/**
 * @brief Receive message from socket.
 */
template <socket_address Addr>
[[nodiscard]] inline auto recvfrom(const file_descriptor& fd, std::span<std::byte> buffer, Addr& src_addr,
    std::int32_t flags = 0) noexcept -> std::expected<std::size_t, std::error_code> {
    socklen_t len { cast::static_converter<socklen_t> {}(sizeof(src_addr)) };
    return expect_result<error_style::posix>(
        ::recvfrom(fd.native_handle(), buffer.data(), buffer.size(), flags, to_sockaddr(src_addr), &len))
        .transform([](auto bytes) noexcept { return toSize(bytes); });
}

/**
 * @brief Convert network address string to binary format.
 */
template <network_address AddrIn>
[[nodiscard]] inline auto inet_pton(std::int32_t af, std::string_view ip, AddrIn& dst) noexcept
    -> std::expected<void, std::error_code> {
    const std::string ip_str { ip };
    return expect_result<error_style::posix>(::inet_pton(af, ip_str.c_str(), static_cast<void*>(&dst)))
        .and_then(
            [](std::int32_t res) noexcept { return expect_success<error_style::pthreads>(res == 0 ? EINVAL : 0); });
}

/**
 * @brief Get options on socket.
 */
template <typename T>
    requires std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>
[[nodiscard]] inline auto getsockopt(const file_descriptor& fd, std::int32_t level, std::int32_t optname,
    T& optval) noexcept -> std::expected<void, std::error_code> {
    socklen_t optlen { cast::static_converter<socklen_t> {}(sizeof(optval)) };
    return expect_result<error_style::posix>(::getsockopt(fd.native_handle(), level, optname, &optval, &optlen))
        .transform([](auto) noexcept {});
}

/**
 * @brief Set options on socket.
 */
template <typename T>
    requires std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>
[[nodiscard]] inline auto setsockopt(const file_descriptor& fd, std::int32_t level, std::int32_t optname,
    const T& optval) noexcept -> std::expected<void, std::error_code> {
    const auto optlen { cast::static_converter<socklen_t> {}(sizeof(optval)) };
    const auto* ptr { static_cast<const void*>(std::addressof(optval)) };
    return expect_result<error_style::posix>(::setsockopt(fd.native_handle(), level, optname, ptr, optlen))
        .transform([](auto) noexcept {});
}

} // namespace posix

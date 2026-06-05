/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "numeric_cast.hpp"
#include "posix_error.hpp"

#include <algorithm>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <limits>
#include <span>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace posix {

/**
 * @brief Select the native @c ioctl(2) request type for the current target libc.
 *
 * POSIX specifies @c int, which @c musl follows.  Standard Linux (glibc)
 * and the kernel headers typically use @c unsigned @c long.
 */
#ifdef __GLIBC__
using native_ioctl_req_t = unsigned long;
#else
using native_ioctl_req_t = int;
#endif

/**
 * @brief Clamp transfer sizes passed to POSIX byte-stream syscalls.
 *
 * POSIX specifies that passing an @c nbyte greater than @c SSIZE_MAX to
 * `read(2)` or `write(2)` yields implementation-defined behavior.
 */
[[nodiscard]] inline constexpr std::size_t max_io_nbyte(std::size_t requested) noexcept {
    constexpr auto kSsizeMax = toSize(std::numeric_limits<ssize_t>::max());
    return std::min(requested, kSsizeMax);
}

template <typename T>
concept non_const = !std::is_const_v<T>;

template <typename T>
concept trivially_copyable = std::is_trivially_copyable_v<T>;

template <typename T>
concept standard_layout = std::is_standard_layout_v<T>;

template <typename F>
concept cancel_callback = std::is_nothrow_invocable_v<std::remove_reference_t<F>>
    && std::convertible_to<std::invoke_result_t<std::remove_reference_t<F>>, bool>;

/**
 * @brief RAII wrapper for a POSIX file descriptor.
 *
 * Move-only, non-copyable.  The managed descriptor is unconditionally
 * closed on destruction.  A descriptor value of @c -1 denotes the
 * empty (invalid) state.
 *
 * @note On Linux, `close(2)` always releases the fd, even when it
 *       returns @c EINTR — retrying would risk closing an unrelated fd
 *       that another thread may have obtained in the meantime.
 *       See: close(2) man page, NOTES section.
 */
class file_descriptor {
public:
    /**
     * @brief POSIX file descriptor native handle.
     * @note Must remain a signed 32-bit integer.
     * @note Required for POSIX error sentinels (e.g. -1) and syscall ABI.
     * @note Do not change this type whatever happens, I mean it!
     */
    using native_handle_type = std::int32_t;

    static constexpr native_handle_type stdin_fd  = STDIN_FILENO;
    static constexpr native_handle_type stdout_fd = STDOUT_FILENO;
    static constexpr native_handle_type stderr_fd = STDERR_FILENO;

    struct write_failure {
        std::size_t bytes_transferred {};
        std::error_code error {};
    };

public:
    /** @brief Error code for an invalid or closed file descriptor. */
    [[nodiscard]] static auto bad_fd_error() noexcept -> std::error_code {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }

    /** @brief Error code for an I/O operation that was canceled. */
    [[nodiscard]] static auto canceled_error() noexcept -> std::error_code {
        return std::make_error_code(std::errc::operation_canceled);
    }

    /** @brief Generic I/O error code. */
    [[nodiscard]] static auto io_error() noexcept -> std::error_code {
        return std::make_error_code(std::errc::io_error);
    }

private:
    native_handle_type fd_ = -1;

    [[nodiscard]] auto ensure_valid_fd() const noexcept -> std::expected<void, std::error_code> {
        if (fd_ < 0) { return std::unexpected(bad_fd_error()); }
        return {};
    }

    [[nodiscard]] auto update_fcntl_flag(std::int32_t get_cmd, std::int32_t set_cmd, std::int32_t flag,
        bool enable) const noexcept -> std::expected<void, std::error_code> {
        if (auto valid = ensure_valid_fd(); !valid) { return std::unexpected(valid.error()); }

        std::int32_t current_flags;
        do {
            current_flags = ::fcntl(fd_, get_cmd, 0);
        } while (current_flags == -1 && errno == EINTR);

        if (current_flags == -1) { return std::unexpected(posix::last_error()); }

        std::int32_t updated_flags = enable ? (current_flags | flag) : (current_flags & ~flag);
        if (updated_flags == current_flags) { return {}; }

        std::int32_t set_res;
        do {
            set_res = ::fcntl(fd_, set_cmd, updated_flags);
        } while (set_res == -1 && errno == EINTR);

        if (set_res == -1) { return std::unexpected(posix::last_error()); }
        return {};
    }

public:
    file_descriptor() = default;

    explicit file_descriptor(native_handle_type raw_fd) noexcept
        : fd_(raw_fd) {}

    ~file_descriptor() noexcept { reset(); }

    file_descriptor(file_descriptor&& other) noexcept
        : fd_(-1) {
        swap(*this, other);
    }

    file_descriptor& operator=(file_descriptor&& other) noexcept {
        if (this != &other) {
            reset();
            swap(*this, other);
        }
        return *this;
    }

    file_descriptor(const file_descriptor&)            = delete;
    file_descriptor& operator=(const file_descriptor&) = delete;

    /**
     * @brief Open a file using standard POSIX flags.
     * @note Atomically adds @c O_CLOEXEC for security.
     */
    [[nodiscard]] static auto open(const std::filesystem::path& path, std::int32_t flags, mode_t mode = 0) noexcept
        -> std::expected<file_descriptor, std::error_code> {
        std::int32_t fd;
        do {
            fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
        } while (fd == -1 && errno == EINTR);
        return from_raw(fd);
    }

    /**
     * @brief Open a file relative to a directory file descriptor (Linux/POSIX.1-2008).
     */
    [[nodiscard]] static auto open_at(native_handle_type dir_fd, const std::filesystem::path& path, std::int32_t flags,
        mode_t mode = 0) noexcept -> std::expected<file_descriptor, std::error_code> {
        native_handle_type fd;
        do {
            fd = ::openat(dir_fd, path.c_str(), flags | O_CLOEXEC, mode);
        } while (fd == -1 && errno == EINTR);
        return from_raw(fd);
    }

    /**
     * @brief Validate and wrap a raw fd obtained from a POSIX call.
     *
     * @param raw_fd  Raw file descriptor (may be negative on caller error).
     * @return        The wrapped descriptor, or an error code on failure.
     */
    [[nodiscard]] static auto from_raw(native_handle_type raw_fd) noexcept
        -> std::expected<file_descriptor, std::error_code> {
        const std::int32_t saved_errno = errno;
        if (raw_fd < 0) { return std::unexpected(saved_errno != 0 ? posix::make_error(saved_errno) : bad_fd_error()); }
        return file_descriptor(raw_fd);
    }

    /**
     * @brief Close the current descriptor and optionally adopt @p new_raw_fd.
     *
     * @c close(2) errors are intentionally ignored — on Linux the fd is
     * always released regardless of the return value, and propagating
     * errors from a destructor path is impractical.
     */
    void reset(native_handle_type new_raw_fd = -1) noexcept {
        if (fd_ >= 0) { ::close(fd_); }
        fd_ = new_raw_fd;
    }

    /** @brief Relinquish ownership and return the raw fd. */
    auto release() noexcept -> native_handle_type { return std::exchange(fd_, -1); }

    /**
     * @brief Create an independent copy of the file descriptor.
     *
     * @param close_on_exec If true, the new descriptor will have FD_CLOEXEC set
     *                      atomically using F_DUPFD_CLOEXEC.
     * @return              The wrapped descriptor, or the current @c errno.
     */
    [[nodiscard]] auto duplicate(bool close_on_exec = true) const noexcept
        -> std::expected<file_descriptor, std::error_code> {
        if (auto valid = ensure_valid_fd(); !valid) { return std::unexpected(valid.error()); }

        std::int32_t copied_fd;
        do {
            copied_fd = close_on_exec ? ::fcntl(fd_, F_DUPFD_CLOEXEC, 0) : ::dup(fd_);
        } while (copied_fd == -1 && errno == EINTR);

        if (copied_fd < 0) { return std::unexpected(posix::last_error()); }
        return file_descriptor(copied_fd);
    }

    /**
     * @brief Redirect this descriptor onto @p target_fd.
     *
     * Uses @c dup3(2) where possible for atomic @c O_CLOEXEC setting.
     *
     * @param target_fd     The target descriptor index to occupy.
     * @param close_on_exec Whether the redirected fd should be closed on exec.
     * @return              Success or a system error code.
     */
    [[nodiscard]] auto redirect_to(native_handle_type target_fd, bool close_on_exec = true) const noexcept
        -> std::expected<void, std::error_code> {
        if (auto valid = ensure_valid_fd(); !valid) { return std::unexpected(valid.error()); }

        if (fd_ == target_fd) { return set_cloexec(close_on_exec); }

        /** @note @c dup3 is atomic and allows setting @c O_CLOEXEC. */
        std::int32_t res;
        do {
            res = ::dup3(fd_, target_fd, close_on_exec ? O_CLOEXEC : 0);
        } while (res == -1 && errno == EINTR);

        if (res == -1) { return std::unexpected(posix::last_error()); }
        return {};
    }

    /**
     * @brief Toggle the @c O_NONBLOCK status flag.
     * @param enable If true, sets O_NONBLOCK; otherwise clears it.
     */
    [[nodiscard]] auto set_nonblocking(bool enable) const noexcept -> std::expected<void, std::error_code> {
        return update_fcntl_flag(F_GETFL, F_SETFL, O_NONBLOCK, enable);
    }

    /**
     * @brief Toggle the @c FD_CLOEXEC file descriptor flag.
     * @param enable If true, sets FD_CLOEXEC; otherwise clears it.
     */
    [[nodiscard]] auto set_cloexec(bool enable) const noexcept -> std::expected<void, std::error_code> {
        return update_fcntl_flag(F_GETFD, F_SETFD, FD_CLOEXEC, enable);
    }

    /** @brief Check if this descriptor is associated with a terminal (TTY). */
    [[nodiscard]] auto is_tty() const noexcept -> bool { return fd_ >= 0 && ::isatty(fd_) == 1; }

    /** @brief Retrieve the size of the file in bytes via fstat(2). */
    [[nodiscard]] auto get_size() const noexcept -> std::expected<off_t, std::error_code> {
        return stat().transform([](const struct ::stat& st) { return st.st_size; });
    }

    /** @brief File metadata via fstat(2). */
    [[nodiscard]] auto stat() const noexcept -> std::expected<struct ::stat, std::error_code> {
        if (auto valid = ensure_valid_fd(); !valid) { return std::unexpected(valid.error()); }
        struct ::stat st {};
        std::int32_t res;
        do {
            res = ::fstat(fd_, &st);
        } while (res == -1 && errno == EINTR);

        if (res == -1) { return std::unexpected(posix::last_error()); }
        return st;
    }

    /**
     * @brief Change file permissions via fchmod(2).
     * @param mode New permission bits (e.g. @c S_IRWXU | @c S_IRGRP).
     */
    [[nodiscard]] auto chmod(mode_t mode) const noexcept -> std::expected<void, std::error_code> {
        if (auto valid = ensure_valid_fd(); !valid) { return std::unexpected(valid.error()); }
        std::int32_t res;
        do {
            res = ::fchmod(fd_, mode);
        } while (res == -1 && errno == EINTR);

        if (res == -1) { return std::unexpected(posix::last_error()); }
        return {};
    }

    /** @brief Reposition the file offset via lseek(2). */
    [[nodiscard]] auto seek(off_t offset, std::int32_t whence) const noexcept -> std::expected<off_t, std::error_code> {
        if (auto valid = ensure_valid_fd(); !valid) { return std::unexpected(valid.error()); }
        off_t res;
        do {
            res = ::lseek(fd_, offset, whence);
        } while (res == -1 && errno == EINTR);

        if (res == -1) { return std::unexpected(posix::last_error()); }
        return res;
    }

    /** @brief Retrieve the current file offset via lseek(2). */
    [[nodiscard]] auto tell() const noexcept -> std::expected<off_t, std::error_code> { return seek(0, SEEK_CUR); }

    /**
     * @brief Type-safe read from the file descriptor.
     *
     * Handles @c EINTR retries and clamps the request to @c SSIZE_MAX.
     * @return The number of bytes read, or a system error code.
     */
    [[nodiscard]] auto read(std::span<std::byte> buffer) const noexcept -> std::expected<std::size_t, std::error_code> {
        return read_raw(fd_, buffer);
    }

    /**
     * @brief Type-safe read with cancellation hook.
     *
     * Invokes @p should_cancel between retries.
     */
    template <cancel_callback CancelCallable>
    [[nodiscard]] auto read(std::span<std::byte> buffer, CancelCallable&& should_cancel) const noexcept
        -> std::expected<std::size_t, std::error_code> {
        return read_raw(fd_, buffer, std::forward<CancelCallable>(should_cancel));
    }

    /** @overload Automatically wraps arbitrary spans into byte spans. */
    template <non_const T> [[nodiscard]] auto read(std::span<T> buffer) const noexcept {
        return read(std::as_writable_bytes(buffer));
    }

    /**
     * @brief Read until buffer is full or EOF.
     */
    [[nodiscard]] auto read_exact(std::span<std::byte> buffer) const noexcept
        -> std::expected<std::size_t, std::error_code> {
        return read_exact_raw(fd_, buffer);
    }

    /**
     * @brief Read exact with cancellation.
     */
    template <cancel_callback CancelCallable>
    [[nodiscard]] auto read_exact(std::span<std::byte> buffer, CancelCallable&& should_cancel) const noexcept
        -> std::expected<std::size_t, std::error_code> {
        return read_exact_raw(fd_, buffer, std::forward<CancelCallable>(should_cancel));
    }

    /** @overload Automatically wraps arbitrary spans into byte spans. */
    template <non_const T> [[nodiscard]] auto read_exact(std::span<T> buffer) const noexcept {
        return read_exact(std::as_writable_bytes(buffer));
    }

    /**
     * @brief Type-safe full write to the file descriptor.
     */
    [[nodiscard]] auto write(std::span<const std::byte> data) const noexcept -> std::expected<void, std::error_code> {
        return write_raw(fd_, data);
    }

    /**
     * @brief Type-safe full write that preserves progress on failure.
     *
     * @return Number of bytes written on success, or a progress+error bundle.
     */
    [[nodiscard]] auto write_exact(std::span<const std::byte> data) const noexcept
        -> std::expected<std::size_t, write_failure> {
        return write_exact_raw(fd_, data);
    }

    /** @overload Automatically wraps arbitrary data into byte spans. */
    template <trivially_copyable T> [[nodiscard]] auto write(std::span<const T> data) const noexcept {
        return write(std::as_bytes(data));
    }

    /** @brief Flush data + metadata via fsync(2). */
    [[nodiscard]] auto sync() const noexcept -> std::expected<void, std::error_code> { return sync_raw(fd_); }

    /** @brief Flush data only via fdatasync(2). */
    [[nodiscard]] auto datasync() const noexcept -> std::expected<void, std::error_code> { return datasync_raw(fd_); }

    /**
     * @brief Set socket options via setsockopt(2).
     *
     * @tparam T        Data type of the option value (must be standard layout).
     * @param level     Protocol level (e.g., SOL_SOCKET).
     * @param optname   Option name (e.g., SO_REUSEADDR).
     * @param value     The value to set.
     * @return          Success or the captured @c errno.
     */
    template <standard_layout T>
    [[nodiscard]] auto setsockopt(std::int32_t level, std::int32_t optname, const T& value) const noexcept
        -> std::expected<void, std::error_code> {
        return setsockopt_raw(fd_, level, optname, value);
    }

    /** @overload Support for raw pointer/size for special cases or buffers. */
    [[nodiscard]] auto setsockopt(std::int32_t level, std::int32_t optname, const void* optval,
        socklen_t optlen) const noexcept -> std::expected<void, std::error_code> {
        return setsockopt_raw(fd_, level, optname, optval, optlen);
    }

    /**
     * @brief Type-safe raw @c ioctl(2) for commands without arguments.
     *
     * @param fd    The raw file descriptor.
     * @param req   The ioctl request code.
     * @return      Success or the captured @c errno.
     */
    [[nodiscard]] static auto ioctl_raw(native_handle_type fd, std::integral auto req) noexcept
        -> std::expected<void, std::error_code> {
        if (fd < 0) { return std::unexpected(bad_fd_error()); }
        std::int32_t res;
        do {
            res = ::ioctl(fd, static_cast<native_ioctl_req_t>(req));
        } while (res == -1 && errno == EINTR);

        if (res == -1) { return std::unexpected(posix::last_error()); }
        return {};
    }

    /**
     * @brief Type-safe raw @c ioctl(2) on an unowned file descriptor.
     */
    template <trivially_copyable T>
    [[nodiscard]] static auto ioctl_raw(native_handle_type fd, std::integral auto req, T& arg) noexcept
        -> std::expected<void, std::error_code> {
        if (fd < 0) { return std::unexpected(bad_fd_error()); }
        std::int32_t res;
        do {
            res = ::ioctl(fd, static_cast<native_ioctl_req_t>(req), &arg);
        } while (res == -1 && errno == EINTR);

        if (res == -1) { return std::unexpected(posix::last_error()); }
        return {};
    }

    /**
     * @brief Type-safe @c ioctl(2) on the managed file descriptor for commands without arguments.
     *
     * @param req   The ioctl request code (e.g. @c SYNC).
     * @return      Success or an error code on failure.
     */
    [[nodiscard]] auto ioctl(std::integral auto req) const noexcept -> std::expected<void, std::error_code> {
        return ioctl_raw(fd_, req);
    }

    /**
     * @brief Type-safe @c ioctl(2) on the managed file descriptor.
     *
     * Eliminates the need to extract a raw @c int from the RAII wrapper.
     * The argument @p arg is passed as a pointer to the kernel as required
     * by the ioctl ABI; the caller never sees the raw fd or the pointer cast.
     *
     * @tparam T    Deduced argument type; must be trivially copyable.
     * @param req   The ioctl request code (e.g. @c BLKSSZGET).
     * @param arg   In/out argument that the kernel reads or writes.
     * @return      Success or an error code on failure.
     */
    template <trivially_copyable T>
    [[nodiscard]] auto ioctl(std::integral auto req, T& arg) const noexcept -> std::expected<void, std::error_code> {
        return ioctl_raw(fd_, req, arg);
    }

    friend void swap(file_descriptor& a, file_descriptor& b) noexcept { std::swap(a.fd_, b.fd_); }

    [[nodiscard]] auto native_handle(this auto&& self) noexcept -> native_handle_type { return self.fd_; }

    explicit operator bool(this auto&& self) noexcept { return self.fd_ >= 0; }

    /**
     * @brief Low-level read from a raw file descriptor.
     *
     * @param fd     The raw file descriptor to read from.
     * @param buffer The destination buffer.
     * @return       Number of bytes read or a system error code.
     * @note         Handles EINTR retries and clamps the request to SSIZE_MAX.
     */
    [[nodiscard]] static auto read_raw(native_handle_type fd, std::span<std::byte> buffer) noexcept
        -> std::expected<std::size_t, std::error_code> {
        return read_raw(fd, buffer, []() noexcept { return false; });
    }

    /**
     * @brief Low-level read with cancellation.
     *
     * @tparam CancelCallable Type of the cancellation callback.
     * @param fd            The raw file descriptor to read from.
     * @param buffer        The destination buffer.
     * @param should_cancel Callback invoked between retries to check for cancellation.
     * @return              Number of bytes read, a system error, or @c errc::operation_canceled.
     * @note                Automatically handles EINTR retries.
     */
    template <cancel_callback CancelCallable>
    [[nodiscard]] static auto read_raw(native_handle_type fd, std::span<std::byte> buffer,
        CancelCallable&& should_cancel) noexcept -> std::expected<std::size_t, std::error_code> {
        if (fd < 0) { return std::unexpected(bad_fd_error()); }
        while (true) {
            if (should_cancel()) { return std::unexpected(canceled_error()); }
            auto bytes_read = ::read(fd, buffer.data(), max_io_nbyte(buffer.size()));
            if (bytes_read >= 0) { return toSize(bytes_read); }
            if (errno != EINTR) { return std::unexpected(posix::last_error()); }
        }
    }

    /**
     * @brief Read until a buffer is fully populated or EOF is reached.
     *
     * @param fd     The raw file descriptor to read from.
     * @param buffer The destination buffer.
     * @return       Number of bytes successfully read or a system error.
     */
    [[nodiscard]] static auto read_exact_raw(native_handle_type fd, std::span<std::byte> buffer) noexcept
        -> std::expected<std::size_t, std::error_code> {
        return read_exact_raw(fd, buffer, []() noexcept { return false; });
    }

    /**
     * @brief Read exact with cancellation.
     *
     * @tparam CancelCallable Type of the cancellation callback.
     * @param fd            The raw file descriptor to read from.
     * @param buffer        The destination buffer.
     * @param should_cancel Callback invoked between retries to check for cancellation.
     * @return              Number of bytes successfully read, or a system error.
     */
    template <cancel_callback CancelCallable>
    [[nodiscard]] static auto read_exact_raw(native_handle_type fd, std::span<std::byte> buffer,
        CancelCallable&& should_cancel) noexcept -> std::expected<std::size_t, std::error_code> {
        std::size_t total_bytes = 0;
        while (total_bytes < buffer.size()) {
            auto bytes_read = read_raw(fd, buffer.subspan(total_bytes), should_cancel);
            if (!bytes_read) { return std::unexpected(bytes_read.error()); }
            if (*bytes_read == 0) { break; }
            total_bytes += *bytes_read;
        }
        return total_bytes;
    }

    /**
     * @brief Perform one raw @c write(2) call.
     *
     * @param fd   The raw file descriptor to write to.
     * @param data The buffer containing data to be written.
     * @return     Number of bytes written by this syscall or a system error.
     * @note       This function intentionally does not retry on @c EINTR.
     *             Retry policy is handled by higher-level loops (e.g. @ref write_exact_raw)
     *             so callers can preserve and report partial progress deterministically.
     */
    [[nodiscard]] static auto write_once_raw(native_handle_type fd, std::span<const std::byte> data) noexcept
        -> std::expected<std::size_t, std::error_code> {
        if (fd < 0) { return std::unexpected(bad_fd_error()); }

        auto bytes_written = ::write(fd, data.data(), max_io_nbyte(data.size()));
        if (bytes_written >= 0) { return toSize(bytes_written); }
        return std::unexpected(posix::last_error());
    }

    /**
     * @brief Full write loop for raw file descriptors with progress reporting.
     *
     * @param fd   The raw file descriptor to write to.
     * @param data The buffer containing data to be written.
     * @return     Total bytes written, or partial progress with the terminal error.
     * @note       Retries transparently on @c EINTR and preserves bytes already transferred.
     */
    [[nodiscard]] static auto write_exact_raw(native_handle_type fd, std::span<const std::byte> data) noexcept
        -> std::expected<std::size_t, write_failure> {
        if (fd < 0) { return std::unexpected(write_failure { 0, bad_fd_error() }); }

        std::size_t total_bytes = 0;
        while (!data.empty()) {
            auto write_result = write_once_raw(fd, data);
            if (!write_result) {
                if (write_result.error() == std::errc::interrupted) { continue; }
                return std::unexpected(write_failure { total_bytes, write_result.error() });
            }

            if (*write_result == 0) { return std::unexpected(write_failure { total_bytes, io_error() }); }

            data = data.subspan(*write_result);
            total_bytes += *write_result;
        }

        return total_bytes;
    }

    /**
     * @brief Backward-compatible full write loop.
     *
     * Preserves previous API by returning only success/error and discarding
     * partial-byte progress details.
     */
    [[nodiscard]] static auto write_raw(native_handle_type fd, std::span<const std::byte> data) noexcept
        -> std::expected<void, std::error_code> {
        auto write_result = write_exact_raw(fd, data);
        if (!write_result) { return std::unexpected(write_result.error().error); }
        return {};
    }

    /**
     * @brief Type-safe raw @c setsockopt(2) on an unowned file descriptor.
     *
     * @tparam T        Data type of the option value (must be standard layout).
     * @param fd        The raw file descriptor.
     * @param level     Protocol level.
     * @param optname   Option name.
     * @param value     The value to set.
     * @return          Success or the captured @c errno.
     */
    template <standard_layout T>
    [[nodiscard]] static auto setsockopt_raw(native_handle_type fd, std::int32_t level, std::int32_t optname,
        const T& value) noexcept -> std::expected<void, std::error_code> {
        if (fd < 0) { return std::unexpected(bad_fd_error()); }
        std::int32_t res;
        do {
            res = ::setsockopt(fd, level, optname, std::addressof(value), sizeof(T));
        } while (res == -1 && errno == EINTR);

        if (res != 0) { return std::unexpected(posix::last_error()); }
        return {};
    }

    /** @overload Support for raw pointer/size for special cases or buffers. */
    [[nodiscard]] static auto setsockopt_raw(native_handle_type fd, std::int32_t level, std::int32_t optname,
        const void* optval, socklen_t optlen) noexcept -> std::expected<void, std::error_code> {
        if (fd < 0) { return std::unexpected(bad_fd_error()); }
        std::int32_t res;
        do {
            res = ::setsockopt(fd, level, optname, optval, optlen);
        } while (res == -1 && errno == EINTR);

        if (res != 0) { return std::unexpected(posix::last_error()); }
        return {};
    }

    /**
     * @brief Flush file data and metadata using @c fsync(2).
     *
     * @param fd The raw file descriptor to sync.
     * @return   Success or a system error.
     * @note     Automatically handles EINTR retries per POSIX specification.
     */
    [[nodiscard]] static auto sync_raw(native_handle_type fd) noexcept -> std::expected<void, std::error_code> {
        if (fd < 0) { return std::unexpected(bad_fd_error()); }
        while (true) {
            if (::fsync(fd) == 0) { return {}; }
            if (errno != EINTR) { return std::unexpected(posix::last_error()); }
        }
    }

    /**
     * @brief Flush file data (but not necessarily metadata) using @c fdatasync(2).
     *
     * @param fd The raw file descriptor to sync.
     * @return   Success or a system error.
     * @note     Automatically handles EINTR retries.
     */
    [[nodiscard]] static auto datasync_raw(native_handle_type fd) noexcept -> std::expected<void, std::error_code> {
        if (fd < 0) { return std::unexpected(bad_fd_error()); }
        while (true) {
            if (::fdatasync(fd) == 0) { return {}; }
            if (errno != EINTR) { return std::unexpected(posix::last_error()); }
        }
    }
};

} // namespace posix

/** @brief Enable std::format and std::print for posix::file_descriptor. */
template <> struct std::formatter<posix::file_descriptor> : std::formatter<std::int32_t> {
    auto format(const posix::file_descriptor& fd, std::format_context& ctx) const {
        return std::formatter<std::int32_t>::format(fd.native_handle(), ctx);
    }
};

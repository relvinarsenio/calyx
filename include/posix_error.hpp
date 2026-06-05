/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "numeric_cast.hpp"

#include <bit>
#include <cerrno>
#include <concepts>
#include <expected>
#include <system_error>
#include <type_traits>

/**
 * @file posix_error.hpp
 * @brief Infrastructure-critical leaf node for POSIX error mapping.
 *
 * @important This header is a fundamental dependency for all system wrappers.
 * To prevent circular dependencies, it MUST NOT include other headers from the
 * project except for base utilities like numeric_cast.hpp.
 */
namespace posix {

/**
 * @brief Translate a raw POSIX/C error code into a std::error_code.
 *
 * This utility provides a centralized mapping between system-level error
 * integers (e.g., errno values) and the modern C++ error reporting framework.
 * It is independent of specific resource abstractions like file descriptors.
 *
 * @param err The raw error code (e.g., EINVAL, ENOMEM).
 * @return    A std::error_code using the system category.
 */
[[nodiscard]] inline auto make_error(std::integral auto err) noexcept -> std::error_code {
    const std::int32_t val = toInt(err);
    return std::error_code(val < 0 ? -val : val, std::system_category());
}

/**
 * @brief Capture the current global errno as a std::error_code.
 *
 * Useful for operations that report failure by setting the global errno
 * variable (standard POSIX behavior).
 *
 * @return A std::error_code representing the current errno.
 */
[[nodiscard]] inline auto last_error() noexcept -> std::error_code {
    return make_error(errno);
}

/**
 * @brief Error reporting styles for different system APIs.
 */
enum class error_style {
    /** @brief Standard POSIX: returns -1, actual error in @c errno. */
    posix,
    /** @brief Linux Internal / io_uring: returns negative errno (e.g., -EAGAIN). */
    linux_internal,
    /** @brief Pointer-based: returns @c nullptr on failure, error in @c errno. */
    pointer,
    /** @brief Pthreads style: returns positive errno on failure, 0 on success. */
    pthreads
};

/**
 * @concept pointer_type
 * @brief Identifies raw pointer types to apply pointer-specific checking policies.
 */
template <typename T>
concept pointer_type = std::is_pointer_v<T>;

/**
 * @concept valid_error_style_type
 * @brief Validates compatibility between error style and API return type at compile time.
 */
template <typename T, error_style Style>
concept valid_error_style_type = (Style == error_style::pointer && pointer_type<T>)
    || ((Style == error_style::posix || Style == error_style::linux_internal) && std::signed_integral<T>)
    || (Style == error_style::pthreads && std::integral<T>);

/**
 * @brief Wraps pointer-based system results (unifying nullptr and MAP_FAILED sentinels).
 * @note Captures errno immediately to prevent state corruption.
 */
template <error_style Style, pointer_type T>
    requires(Style == error_style::pointer)
[[nodiscard]] constexpr auto expect_result(T res) noexcept -> std::expected<T, std::error_code> {
    if (res != nullptr && std::bit_cast<std::uintptr_t>(res) != static_cast<std::uintptr_t>(-1)) [[likely]] {
        return res;
    }
    const std::int32_t err = errno;
    return std::unexpected(make_error(err ? err : ENOMEM));
}

/**
 * @brief Wraps standard POSIX results indicating failure via a -1 sentinel.
 * @note Captures errno immediately to prevent state corruption.
 */
template <error_style Style, std::signed_integral T>
    requires(Style == error_style::posix)
[[nodiscard]] constexpr auto expect_result(T res) noexcept -> std::expected<T, std::error_code> {
    if (res != -1) [[likely]] { return res; }
    return std::unexpected(last_error());
}

/**
 * @brief Wraps direct syscall/kernel results returning negative error codes (e.g. io_uring).
 * @note Avoids TLS/errno lookup overhead by extracting the error code directly.
 */
template <error_style Style, std::signed_integral T>
    requires(Style == error_style::linux_internal)
[[nodiscard]] constexpr auto expect_result(T res) noexcept -> std::expected<T, std::error_code> {
    if (res >= 0) [[likely]] { return res; }
    return std::unexpected(make_error(res));
}

/**
 * @brief Wraps pthread-style API results returning positive error codes on failure.
 * @note Pthread APIs do not set errno, but return the status code directly.
 */
template <error_style Style, std::integral T>
    requires(Style == error_style::pthreads)
[[nodiscard]] constexpr auto expect_result(T res) noexcept -> std::expected<T, std::error_code> {
    if (res == 0) [[likely]] { return res; }
    return std::unexpected(make_error(res));
}

/**
 * @brief Discards successful values to return a monadic expected-void representation.
 */
template <error_style Style, valid_error_style_type<Style> T>
[[nodiscard]] constexpr auto expect_success(T res) noexcept -> std::expected<void, std::error_code> {
    return expect_result<Style>(res).transform([](auto) {});
}

} // namespace posix

/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "numeric_cast.hpp"

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
 * @brief Premium Monadic I/O Helper: Converts various C-style error patterns to std::expected.
 *
 * @tparam Style The error reporting policy to apply.
 * @tparam T     The return type of the C API.
 * @param res    The raw result from the system/library call.
 * @return       Success containing the result, or a captured std::error_code.
 */
template <error_style Style, typename T>
[[nodiscard]] constexpr auto expect_result(T res) noexcept -> std::expected<T, std::error_code> {
    if constexpr (Style == error_style::pointer) {
        static_assert(std::is_pointer_v<T>, "pointer style requires a pointer type");
        if (res != nullptr) [[likely]] { return res; }
        const std::int32_t err = errno;
        return std::unexpected(make_error(err ? err : ENOMEM));
    } else if constexpr (Style == error_style::posix) {
        static_assert(std::is_signed_v<T>, "posix style requires a signed integer type");
        if (res != -1) [[likely]] { return res; }
        return std::unexpected(last_error());
    } else if constexpr (Style == error_style::linux_internal) {
        static_assert(std::is_signed_v<T>, "linux_internal style requires a signed integer type");
        if (res >= 0) [[likely]] { return res; }
        return std::unexpected(make_error(res));
    } else if constexpr (Style == error_style::pthreads) {
        static_assert(std::is_integral_v<T>, "pthreads style requires an integral type");
        if (res == 0) [[likely]] { return res; }
        return std::unexpected(make_error(res));
    }
}

/** @brief Alias for functions where only success/failure matters. */
template <error_style Style, typename T>
[[nodiscard]] constexpr auto expect_success(T res) noexcept -> std::expected<void, std::error_code> {
    return expect_result<Style>(res).transform([](auto) {});
}

} // namespace posix

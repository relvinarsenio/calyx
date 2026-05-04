/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

/**
 * @namespace cast
 * @brief Core library namespace for the Calyx system utilities.
 */
namespace cast {

/**
 * @brief Matches standard and extended signed/unsigned integer types.
 *
 * Matches the C++26 definition of "standard integer types" and "extended integer types".
 * Explicitly excludes character types (char, wchar_t, char8_t, char16_t, char32_t)
 * and bool as required by the P0543R3 specification for saturation arithmetic.
 */
template <class T>
concept standard_integer_type = std::is_integral_v<std::remove_cvref_t<T>>
    && (!std::is_same_v<std::remove_cvref_t<T>, bool>) && (!std::is_same_v<std::remove_cvref_t<T>, char>)
    && (!std::is_same_v<std::remove_cvref_t<T>, wchar_t>) && (!std::is_same_v<std::remove_cvref_t<T>, char8_t>)
    && (!std::is_same_v<std::remove_cvref_t<T>, char16_t>) && (!std::is_same_v<std::remove_cvref_t<T>, char32_t>);

/**
 * @brief Matches any arithmetic type except bool.
 */
template <class T>
concept numeric_type = std::is_arithmetic_v<std::remove_cvref_t<T>> && (!std::is_same_v<std::remove_cvref_t<T>, bool>);

namespace detail {

/**
 * @brief Normalizes a character or integral type to a standard integer for std::cmp_* safety.
 *
 * Traditional C++ character types are often excluded from strict integer comparison utilities.
 * This helper ensures they are treated as their underlying signed or unsigned byte equivalents.
 */
template <class T> constexpr auto to_std_int(T value) noexcept {
    if constexpr (std::is_same_v<T, char>) {
        return static_cast<std::conditional_t<std::is_signed_v<char>, signed char, unsigned char>>(value);
    } else if constexpr (std::is_same_v<T, wchar_t>) {
        return static_cast<
            std::conditional_t<std::is_signed_v<wchar_t>, std::make_signed_t<wchar_t>, std::make_unsigned_t<wchar_t>>>(
            value);
    } else if constexpr (std::is_same_v<T, char8_t>) {
        return static_cast<std::uint_least8_t>(value);
    } else if constexpr (std::is_same_v<T, char16_t>) {
        return static_cast<std::uint_least16_t>(value);
    } else if constexpr (std::is_same_v<T, char32_t>) {
        return static_cast<std::uint_least32_t>(value);
    } else {
        return value;
    }
}

/**
 * @brief Performs saturating conversion from floating-point values to integral types.
 *
 * Follows professional rounding and saturation standards. Handles NaNs by returning 0.
 */
template <class T, class U>
    requires std::is_integral_v<T> && std::is_floating_point_v<U>
[[nodiscard]] constexpr T saturate_fp_to_int(U x) noexcept {
    if (std::isnan(x)) { return T { 0 }; }

    static_assert(sizeof(T) <= 8, "Unsupported target integer width for floating-point saturation.");

    constexpr U kMaxBound = static_cast<U>((sizeof(T) == 8) ? (std::is_signed_v<T> ? 0x1p63 : 0x1p64)
            : (sizeof(T) == 4)                              ? (std::is_signed_v<T> ? 0x1p31 : 0x1p32)
            : (sizeof(T) == 2)                              ? (std::is_signed_v<T> ? 0x1p15 : 0x1p16)
                                                            : (std::is_signed_v<T> ? 0x1p7 : 0x1p8));

    constexpr U kMinBound = std::is_signed_v<T> ? -kMaxBound : U { 0 };

    if (x >= kMaxBound) { return std::numeric_limits<T>::max(); }
    if (x <= kMinBound) { return std::numeric_limits<T>::min(); }

    return static_cast<T>(x);
}

/**
 * @brief Internal engine for saturating casts supporting mixed integral and floating-point types.
 */
template <class T, class U>
    requires numeric_type<T> && numeric_type<U>
[[nodiscard]] constexpr T saturating_cast_impl(U x) noexcept {
    if constexpr (std::is_same_v<T, U>) {
        return x;
    } else if constexpr (std::is_integral_v<T> && std::is_integral_v<U>) {
        auto sx    = to_std_int(x);
        auto t_min = to_std_int(std::numeric_limits<T>::min());
        auto t_max = to_std_int(std::numeric_limits<T>::max());

        if (std::cmp_greater(sx, t_max)) { return std::numeric_limits<T>::max(); }
        if (std::cmp_less(sx, t_min)) { return std::numeric_limits<T>::min(); }
        return static_cast<T>(x);
    } else if constexpr (std::is_integral_v<T> && std::is_floating_point_v<U>) {
        return saturate_fp_to_int<T>(x);
    } else {
        return static_cast<T>(x);
    }
}

} // namespace detail

/**
 * @brief Performs a saturating conversion between standard integer types.
 *
 * This implementation is a polyfill for the C++26 std::saturate_cast (P0543R3).
 * It converts the value x to type T while clamping the result to the representable
 * range of T. This prevents undefined behavior associated with integer overflow.
 *
 * @tparam T The target standard integer type.
 * @tparam U The source standard integer type.
 * @param x The value to convert.
 * @return The converted value, saturated to the range of T.
 */
template <standard_integer_type T, standard_integer_type U> [[nodiscard]] constexpr T saturate_cast(U x) noexcept {
    if (std::cmp_greater(x, std::numeric_limits<T>::max())) { return std::numeric_limits<T>::max(); }
    if (std::cmp_less(x, std::numeric_limits<T>::min())) { return std::numeric_limits<T>::min(); }
    return static_cast<T>(x);
}

} // namespace cast

/**
 * @name Kotlin/Rust-style numeric conversions
 * @brief Expressive aliases for saturating and unchecked casting of arithmetic types.
 * @{
 */

template <cast::numeric_type T> [[nodiscard]] constexpr auto toByte(T value) noexcept {
    return cast::detail::saturating_cast_impl<std::int8_t>(value);
}
template <cast::numeric_type T> [[nodiscard]] constexpr auto toShort(T value) noexcept {
    return cast::detail::saturating_cast_impl<std::int16_t>(value);
}
template <cast::numeric_type T> [[nodiscard]] constexpr auto toInt(T value) noexcept {
    return cast::detail::saturating_cast_impl<std::int32_t>(value);
}
template <cast::numeric_type T> [[nodiscard]] constexpr auto toLong(T value) noexcept {
    return cast::detail::saturating_cast_impl<std::int64_t>(value);
}

template <cast::numeric_type T> [[nodiscard]] constexpr auto toUByte(T value) noexcept {
    return cast::detail::saturating_cast_impl<std::uint8_t>(value);
}
template <cast::numeric_type T> [[nodiscard]] constexpr auto toUShort(T value) noexcept {
    return cast::detail::saturating_cast_impl<std::uint16_t>(value);
}
template <cast::numeric_type T> [[nodiscard]] constexpr auto toUInt(T value) noexcept {
    return cast::detail::saturating_cast_impl<std::uint32_t>(value);
}
template <cast::numeric_type T> [[nodiscard]] constexpr auto toULong(T value) noexcept {
    return cast::detail::saturating_cast_impl<std::uint64_t>(value);
}

template <cast::numeric_type T> [[nodiscard]] constexpr auto toChar(T value) noexcept {
    return cast::detail::saturating_cast_impl<char>(value);
}

/** @brief Bit-preserving cast to unsigned char (non-saturating). */
[[nodiscard]] constexpr auto toUChar(char value) noexcept -> unsigned char {
    return static_cast<unsigned char>(value);
}

template <cast::numeric_type T> [[nodiscard]] constexpr auto toFloat(T value) noexcept {
    return static_cast<float>(value);
}
template <cast::numeric_type T> [[nodiscard]] constexpr auto toDouble(T value) noexcept {
    return static_cast<double>(value);
}
template <cast::numeric_type T> [[nodiscard]] constexpr auto toLongDouble(T value) noexcept {
    return static_cast<long double>(value);
}

/** @brief Specialization targeting std::size_t. */
template <cast::numeric_type T> [[nodiscard]] constexpr auto toSize(T value) noexcept {
    return cast::detail::saturating_cast_impl<std::size_t>(value);
}

/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
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

namespace saturating_impl {

template <std::integral T> struct std_int_mapper {
    using type = T;
};

template <> struct std_int_mapper<char> {
    using type = std::conditional_t<std::is_signed_v<char>, signed char, unsigned char>;
};

template <> struct std_int_mapper<wchar_t> {
    using type
        = std::conditional_t<std::is_signed_v<wchar_t>, std::make_signed_t<wchar_t>, std::make_unsigned_t<wchar_t>>;
};

template <> struct std_int_mapper<char8_t> {
    using type = std::uint_least8_t;
};

template <> struct std_int_mapper<char16_t> {
    using type = std::uint_least16_t;
};

template <> struct std_int_mapper<char32_t> {
    using type = std::uint_least32_t;
};

template <std::integral T> using std_int_mapper_t = typename std_int_mapper<T>::type;

/**
 * @brief Normalizes a character or integral type to a standard integer for std::cmp_* safety.
 *
 * Traditional C++ character types are often excluded from strict integer comparison utilities.
 * This helper ensures they are treated as their underlying signed or unsigned byte equivalents.
 */
template <std::integral T> constexpr auto to_std_int(T value) noexcept {
    return static_cast<std_int_mapper_t<T>>(value);
}

/**
 * @brief Computes 2^n exactly at compile time for any IEEE 754 floating-point type.
 *
 * @note Relies on standard IEEE 754 binary representation where multiplication by 2
 *       is exact and merely increments the exponent without precision loss.
 */
template <std::floating_point U> [[nodiscard]] consteval U ipow2(std::integral auto n) noexcept {
    const U factor = static_cast<U>(2);
    return std::ranges::fold_left(std::views::iota(decltype(n) { 0 }, n), static_cast<U>(1),
        [factor](U acc, auto) noexcept { return acc * factor; });
}

/**
 * @brief Computes the exclusive upper bound (as a floating-point power-of-2) for saturating
 *        conversion from floating-point type U to integer type T.
 *
 * @note Uses exact power-of-2 values (2^digits) to avoid rounding errors that arise from casting
 *       std::numeric_limits<T>::max() directly to a floating-point type.
 */
template <std::integral T, std::floating_point U> [[nodiscard]] consteval U fp_saturation_upper_bound() noexcept {
    static_assert(std::numeric_limits<U>::is_iec559, "Floating-point type must conform to IEEE 754 (IEC 559).");
    static_assert(sizeof(T) <= 8, "Target integer type must be at most 8 bytes (64 bits) wide.");
    return ipow2<U>(std::numeric_limits<T>::digits);
}

/**
 * @brief Performs saturating conversion from floating-point values to integral types.
 *
 * Follows professional rounding and saturation standards. Handles NaNs by returning 0.
 */
template <std::integral T, std::floating_point U> [[nodiscard]] constexpr T saturate_fp_to_int(U x) noexcept {
    if (std::isnan(x)) { return T { 0 }; }

    constexpr U kMaxBound = fp_saturation_upper_bound<T, U>();
    constexpr U kMinBound = std::is_signed_v<T> ? -kMaxBound : U { 0 };

    if (x >= kMaxBound) { return std::numeric_limits<T>::max(); }
    if (x <= kMinBound) { return std::numeric_limits<T>::min(); }

    return static_cast<T>(x);
}

/**
 * @brief Internal engine for saturating casts (Same Type)
 */
template <numeric_type T, numeric_type U>
    requires std::same_as<T, U>
[[nodiscard]] constexpr T saturating_cast_impl(U x) noexcept {
    return x;
}

/**
 * @brief Internal engine for saturating casts (Integral to Integral)
 */
template <numeric_type T, numeric_type U>
    requires std::integral<T> && std::integral<U> && (!std::same_as<T, U>)
[[nodiscard]] constexpr T saturating_cast_impl(U x) noexcept {
    auto sx    = to_std_int(x);
    auto t_min = to_std_int(std::numeric_limits<T>::min());
    auto t_max = to_std_int(std::numeric_limits<T>::max());

    if (std::cmp_greater(sx, t_max)) { return std::numeric_limits<T>::max(); }
    if (std::cmp_less(sx, t_min)) { return std::numeric_limits<T>::min(); }
    return static_cast<T>(x);
}

/**
 * @brief Internal engine for saturating casts (Floating-Point to Integral)
 */
template <numeric_type T, numeric_type U>
    requires std::integral<T> && std::floating_point<U>
[[nodiscard]] constexpr T saturating_cast_impl(U x) noexcept {
    return saturate_fp_to_int<T>(x);
}

/**
 * @brief Internal engine for saturating casts (Integral/Floating to Floating-Point)
 */
template <numeric_type T, numeric_type U>
    requires std::floating_point<T> && (!std::same_as<T, U>)
[[nodiscard]] constexpr T saturating_cast_impl(U x) noexcept {
    return static_cast<T>(x);
}

} // namespace saturating_impl

/**
 * @brief Performs a saturating conversion between standard integer types.
 *
 * This implementation is a polyfill for the C++26 std::saturate_cast (P0543R3).
 * It converts the value x to type T while clamping the result to the representable
 * range of T. This prevents undefined behavior associated with integer overflow.
 */
template <standard_integer_type T, standard_integer_type U> [[nodiscard]] constexpr T saturate_cast(U x) noexcept {
    return saturating_impl::saturating_cast_impl<T>(x);
}

/**
 * @name Kotlin/Rust-style numeric conversions
 * @brief Expressive aliases for saturating and unchecked casting of arithmetic types.
 * @{
 */

/** @brief Functor template for saturating numeric conversions. */
template <numeric_type TargetType> struct saturating_converter {
    template <numeric_type T> [[nodiscard]] constexpr auto operator()(T value) const noexcept {
        return saturating_impl::saturating_cast_impl<TargetType>(value);
    }
};

/** @brief Functor template for standard static conversions. */
template <numeric_type TargetType> struct static_converter {
    template <numeric_type T> [[nodiscard]] constexpr auto operator()(T value) const noexcept {
        return static_cast<TargetType>(value);
    }
};

/** @brief Functor for bit-preserving cast to unsigned char (non-saturating). */
struct to_uchar_converter {
    [[nodiscard]] constexpr auto operator()(char value) const noexcept -> unsigned char {
        return static_cast<unsigned char>(value);
    }
};

} // namespace cast

inline constexpr cast::saturating_converter<std::int8_t> toByte {};
inline constexpr cast::saturating_converter<std::int16_t> toShort {};
inline constexpr cast::saturating_converter<std::int32_t> toInt {};
inline constexpr cast::saturating_converter<std::int64_t> toLong {};

inline constexpr cast::saturating_converter<std::uint8_t> toUByte {};
inline constexpr cast::saturating_converter<std::uint16_t> toUShort {};
inline constexpr cast::saturating_converter<std::uint32_t> toUInt {};
inline constexpr cast::saturating_converter<std::uint64_t> toULong {};

inline constexpr cast::saturating_converter<char> toChar {};
inline constexpr cast::to_uchar_converter toUChar {};

inline constexpr cast::static_converter<float> toFloat {};
inline constexpr cast::static_converter<double> toDouble {};
inline constexpr cast::static_converter<long double> toLongDouble {};

inline constexpr cast::saturating_converter<std::size_t> toSize {};

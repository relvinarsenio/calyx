/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <cmath>
#include <concepts>
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
 * @brief Computes the exclusive upper bound (as a floating-point power-of-2) for saturating
 *        conversion from floating-point type U to integer type T.
 *
 * @note Uses exact power-of-2 values to avoid rounding errors that arise from casting
 *       std::numeric_limits<T>::max() directly to a floating-point type.
 */
template <std::integral T, std::floating_point U> [[nodiscard]] consteval U fp_saturation_upper_bound() noexcept {
    static_assert(sizeof(T) <= 8, "Unsupported target integer width for floating-point saturation.");
    if constexpr (sizeof(T) == 8) {
        return std::is_signed_v<T> ? U { 0x1p63 } : U { 0x1p64 };
    } else if constexpr (sizeof(T) == 4) {
        return std::is_signed_v<T> ? U { 0x1p31 } : U { 0x1p32 };
    } else if constexpr (sizeof(T) == 2) {
        return std::is_signed_v<T> ? U { 0x1p15 } : U { 0x1p16 };
    } else {
        return std::is_signed_v<T> ? U { 0x1p7 } : U { 0x1p8 };
    }
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
 * @brief Internal engine for saturating casts supporting mixed integral and floating-point types.
 */
template <numeric_type T, numeric_type U> [[nodiscard]] constexpr T saturating_cast_impl(U x) noexcept {
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

} // namespace saturating_impl

/**
 * @brief Performs a saturating conversion between standard integer types.
 *
 * This implementation is a polyfill for the C++26 std::saturate_cast (P0543R3).
 * It converts the value x to type T while clamping the result to the representable
 * range of T. This prevents undefined behavior associated with integer overflow.
 */
template <standard_integer_type T, standard_integer_type U> [[nodiscard]] constexpr T saturate_cast(U x) noexcept {
    if (std::cmp_greater(x, std::numeric_limits<T>::max())) { return std::numeric_limits<T>::max(); }
    if (std::cmp_less(x, std::numeric_limits<T>::min())) { return std::numeric_limits<T>::min(); }
    return static_cast<T>(x);
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

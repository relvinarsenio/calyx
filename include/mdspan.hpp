/*
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 * Adapted and modified by Alfie Ardinata for the Calyx project.
 */
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <print>
#include <source_location>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef STX_HIDDEN
#if defined(_MSC_VER)
#define STX_HIDDEN
#elif defined(__GNUC__) || defined(__clang__)
#define STX_HIDDEN __attribute__((__visibility__("hidden")))
#else
#define STX_HIDDEN
#endif
#endif

#ifndef STX_EXCLUDE_FROM_EXPLICIT_INSTANTIATION
#if defined(_MSC_VER)
#define STX_EXCLUDE_FROM_EXPLICIT_INSTANTIATION __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define STX_EXCLUDE_FROM_EXPLICIT_INSTANTIATION __attribute__((__exclude_from_explicit_instantiation__))
#else
#define STX_EXCLUDE_FROM_EXPLICIT_INSTANTIATION [[gnu::always_inline]] inline
#endif
#endif

#ifndef STX_HIDE_FROM_ABI
#if defined(_MSC_VER)
#define STX_HIDE_FROM_ABI __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define STX_HIDE_FROM_ABI STX_HIDDEN STX_EXCLUDE_FROM_EXPLICIT_INSTANTIATION __attribute__((__abi_tag__("stx_v23")))
#else
#define STX_HIDE_FROM_ABI inline
#endif
#endif

#ifndef STX_NO_UNIQUE_ADDRESS
#define STX_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

namespace stx {
namespace __detail {
STX_HIDE_FROM_ABI inline void __handle_assertion_failure(std::string_view __expr, std::string_view __msg,
    const std::source_location __loc = std::source_location::current()) noexcept {
    std::println(stderr, "stx::mdspan assertion failed: {}\nLocation: {}:{} ({})\nExpression: {}\nMessage: {}",
        "Hardening check failed", __loc.file_name(), __loc.line(), __loc.function_name(), __expr, __msg);
    std::abort();
}
} // namespace __detail
} // namespace stx

#ifndef STX_ASSERT
#if defined(NDEBUG)
#define STX_ASSERT(expression, message) (void)(expression)
#else
#define STX_ASSERT(expression, message) \
    (__builtin_expect(static_cast<bool>(expression), 1) \
            ? (void)0 \
            : ::stx::__detail::__handle_assertion_failure(#expression, message))
#endif
#endif

#ifndef STX_ASSERT_VALID_ELEMENT_ACCESS
#define STX_ASSERT_VALID_ELEMENT_ACCESS(expression, message) STX_ASSERT(expression, message)
#endif

#ifndef STX_ASSERT_UNCATEGORIZED
#define STX_ASSERT_UNCATEGORIZED(expression, message) STX_ASSERT(expression, message)
#endif

#ifndef STX_NODEBUG
#if defined(__clang__)
#define STX_NODEBUG __attribute__((__nodebug__))
#else
#define STX_NODEBUG
#endif
#endif

#ifndef STX_UNREACHABLE
#define STX_UNREACHABLE() __builtin_unreachable()
#endif

#ifndef STX_STD_VER
#define STX_STD_VER 23
#endif

namespace stx {

using std::addressof;
using std::array;
using std::as_const;
using std::bool_constant;
using std::common_type_t;
using std::dynamic_extent;
using std::extent_v;
using std::false_type;
using std::forward;
using std::index_sequence;
using std::integral;
using std::integral_constant;
using std::is_abstract_v;
using std::is_array_v;
using std::is_constructible_v;
using std::is_convertible;
using std::is_convertible_v;
using std::is_default_constructible_v;
using std::is_integral;
using std::is_integral_v;
using std::is_nothrow_constructible_v;
using std::is_object_v;
using std::is_pointer_v;
using std::is_same_v;
using std::is_signed_v;
using std::make_index_sequence;
using std::make_unsigned_t;
using std::move;
using std::numeric_limits;
using std::rank_v;
using std::remove_all_extents_t;
using std::remove_cv_t;
using std::remove_cvref_t;
using std::remove_pointer_t;
using std::remove_reference_t;
using std::same_as;
using std::size_t;
using std::span;
using std::swap;
using std::true_type;

#if STX_STD_VER >= 23

// Forward declarations
struct layout_left;
struct layout_right;
struct layout_stride;

template <class _IndexType, size_t... _Extents> class extents;

namespace __mdspan_detail {

template <class _Tp, _Tp... _Values> struct __static_array {
    static constexpr array<_Tp, sizeof...(_Values)> __array = { _Values... };

public:
    STX_HIDE_FROM_ABI static constexpr size_t __size() { return sizeof...(_Values); }
    STX_HIDE_FROM_ABI static constexpr _Tp __get(size_t __index) noexcept { return __array[__index]; }

    template <size_t _Index> STX_HIDE_FROM_ABI static constexpr _Tp __get() { return __get(_Index); }
};

template <class _Tp, size_t _Size> struct __possibly_empty_array {
    _Tp __vals_[_Size];
    STX_HIDE_FROM_ABI constexpr _Tp& operator[](size_t __index) { return __vals_[__index]; }
    STX_HIDE_FROM_ABI constexpr const _Tp& operator[](size_t __index) const { return __vals_[__index]; }
    STX_HIDE_FROM_ABI constexpr _Tp* data() { return __vals_; }
    STX_HIDE_FROM_ABI constexpr const _Tp* data() const { return __vals_; }
};

template <class _Tp> struct __possibly_empty_array<_Tp, 0> {
    STX_HIDE_FROM_ABI constexpr _Tp& operator[](size_t) { STX_UNREACHABLE(); }
    STX_HIDE_FROM_ABI constexpr const _Tp& operator[](size_t) const { STX_UNREACHABLE(); }
    STX_HIDE_FROM_ABI constexpr _Tp* data() { return nullptr; }
    STX_HIDE_FROM_ABI constexpr const _Tp* data() const { return nullptr; }
};

template <size_t... _Values> struct __static_partial_sums {
    STX_HIDE_FROM_ABI static constexpr array<size_t, sizeof...(_Values)> __static_partial_sums_impl() {
        array<size_t, sizeof...(_Values)> __values { _Values... };
        array<size_t, sizeof...(_Values)> __partial_sums { {} };
        size_t __running_sum = 0;
        for (size_t __i = 0; __i != sizeof...(_Values); ++__i) {
            __partial_sums[__i] = __running_sum;
            __running_sum += static_cast<size_t>(__values[__i]);
        }
        return __partial_sums;
    }
    static constexpr array<size_t, sizeof...(_Values)> __result { __static_partial_sums_impl() };

    STX_HIDE_FROM_ABI static constexpr size_t __get(size_t __index) { return __result[__index]; }
};

template <class _TDynamic, class _TStatic, _TStatic _DynTag, _TStatic... _Values> struct __maybe_static_array {
    static_assert(
        is_convertible<_TStatic, _TDynamic>::value, "__maybe_static_array: _TStatic must be convertible to _TDynamic");
    static_assert(
        is_convertible<_TDynamic, _TStatic>::value, "__maybe_static_array: _TDynamic must be convertible to _TStatic");

private:
    static constexpr size_t __size_         = sizeof...(_Values);
    static constexpr size_t __size_dynamic_ = ((_Values == _DynTag) + ... + 0);
    using _StaticValues STX_NODEBUG         = __static_array<_TStatic, _Values...>;
    using _DynamicValues STX_NODEBUG        = __possibly_empty_array<_TDynamic, __size_dynamic_>;

    STX_NO_UNIQUE_ADDRESS _DynamicValues __dyn_vals_;

    using _DynamicIdxMap STX_NODEBUG = __static_partial_sums<static_cast<size_t>(_Values == _DynTag)...>;

    template <size_t... _Indices>
    STX_HIDE_FROM_ABI static constexpr _DynamicValues __zeros(index_sequence<_Indices...>) noexcept {
        return _DynamicValues { ((void)_Indices, 0)... };
    }

public:
    STX_HIDE_FROM_ABI constexpr __maybe_static_array() noexcept
        : __dyn_vals_ { __zeros(make_index_sequence<__size_dynamic_>()) } {}

    template <class... _DynVals>
        requires(sizeof...(_DynVals) == __size_dynamic_)
    STX_HIDE_FROM_ABI constexpr __maybe_static_array(_DynVals... __vals)
        : __dyn_vals_ { static_cast<_TDynamic>(__vals)... } {}

    template <class _Tp, size_t _Size>
        requires(_Size == __size_dynamic_)
    STX_HIDE_FROM_ABI constexpr __maybe_static_array([[maybe_unused]] const span<_Tp, _Size>& __vals) {
        if constexpr (_Size > 0) {
            for (size_t __i = 0; __i < _Size; __i++) {
                __dyn_vals_[__i] = static_cast<_TDynamic>(__vals[__i]);
            }
        }
    }

    template <class... _DynVals>
        requires(sizeof...(_DynVals) != __size_dynamic_)
    STX_HIDE_FROM_ABI constexpr __maybe_static_array(_DynVals... __vals) {
        static_assert(sizeof...(_DynVals) == __size_, "Invalid number of values.");
        _TDynamic __values[__size_] = { static_cast<_TDynamic>(__vals)... };
        for (size_t __i = 0; __i < __size_; __i++) {
            _TStatic __static_val = _StaticValues::__get(__i);
            if (__static_val == _DynTag) {
                __dyn_vals_[_DynamicIdxMap::__get(__i)] = __values[__i];
            } else {
                STX_ASSERT_VALID_ELEMENT_ACCESS(__values[__i] == static_cast<_TDynamic>(__static_val),
                    "extents construction: mismatch of provided arguments with static extents.");
            }
        }
    }

    template <class _Tp, size_t _Size>
        requires(_Size != __size_dynamic_)
    STX_HIDE_FROM_ABI constexpr __maybe_static_array(const span<_Tp, _Size>& __vals) {
        static_assert(_Size == __size_ || __size_ == dynamic_extent);
        for (size_t __i = 0; __i < __size_; __i++) {
            _TStatic __static_val = _StaticValues::__get(__i);
            if (__static_val == _DynTag) {
                __dyn_vals_[_DynamicIdxMap::__get(__i)] = static_cast<_TDynamic>(__vals[__i]);
            } else {
                STX_ASSERT_VALID_ELEMENT_ACCESS(
                    static_cast<_TDynamic>(__vals[__i]) == static_cast<_TDynamic>(__static_val),
                    "extents construction: mismatch of provided arguments with static extents.");
            }
        }
    }

    STX_HIDE_FROM_ABI static constexpr _TStatic __static_value(size_t __i) noexcept {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__i < __size_, "extents access: index must be less than rank");
        return _StaticValues::__get(__i);
    }

    STX_HIDE_FROM_ABI constexpr _TDynamic __value(size_t __i) const {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__i < __size_, "extents access: index must be less than rank");
        _TStatic __static_val = _StaticValues::__get(__i);
        return __static_val == _DynTag ? __dyn_vals_[_DynamicIdxMap::__get(__i)] : static_cast<_TDynamic>(__static_val);
    }
    STX_HIDE_FROM_ABI constexpr _TDynamic operator[](size_t __i) const {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__i < __size_, "extents access: index must be less than rank");
        return __value(__i);
    }

    STX_HIDE_FROM_ABI static constexpr size_t __size() { return __size_; }
    STX_HIDE_FROM_ABI static constexpr size_t __size_dynamic() { return __size_dynamic_; }
};

template <class _Tp> struct __is_integral_constant : false_type {};

template <class _Tp, _Tp _Val> struct __is_integral_constant<integral_constant<_Tp, _Val>> : true_type {};

template <class _Tp>
inline constexpr size_t __maybe_static_ext
    = __is_integral_constant<_Tp>::value ? static_cast<size_t>(_Tp::value) : dynamic_extent;

template <integral _To, class _From>
    requires(integral<_From>)
STX_HIDE_FROM_ABI constexpr bool __is_representable_as(_From __value) {
    using _To_u   = make_unsigned_t<_To>;
    using _From_u = make_unsigned_t<_From>;
    if constexpr (is_signed_v<_From>) {
        if (__value < 0) { return false; }
    }
    if constexpr (static_cast<_To_u>(numeric_limits<_To>::max())
        >= static_cast<_From_u>(numeric_limits<_From>::max())) {
        return true;
    } else {
        return static_cast<_To_u>(numeric_limits<_To>::max()) >= static_cast<_From_u>(__value);
    }
}

template <integral _To, class _From>
    requires(!integral<_From>)
STX_HIDE_FROM_ABI constexpr bool __is_representable_as(_From __value) {
    if constexpr (is_signed_v<_To>) {
        if (static_cast<_To>(__value) < 0) { return false; }
    }
    return true;
}

template <integral _To, class... _From> STX_HIDE_FROM_ABI constexpr bool __are_representable_as(_From... __values) {
    return (__is_representable_as<_To>(__values) && ... && true);
}

template <integral _To, class _From, size_t _Size>
STX_HIDE_FROM_ABI constexpr bool __are_representable_as(span<_From, _Size> __values) {
    for (size_t __i = 0; __i < _Size; __i++) {
        if (!__is_representable_as<_To>(__values[__i])) { return false; }
    }
    return true;
}

template <class _Tp>
concept __signed_or_unsigned_integer = integral<_Tp> && !same_as<_Tp, bool>;

template <integral _IndexType, class _From>
    requires(integral<_From>)
STX_HIDE_FROM_ABI constexpr bool __is_index_in_extent(_IndexType __extent, _From __value) {
    if constexpr (is_signed_v<_From>) {
        if (__value < 0) { return false; }
    }
    using _Tp = common_type_t<_IndexType, _From>;
    return static_cast<_Tp>(__value) < static_cast<_Tp>(__extent);
}

template <integral _IndexType, class _From>
    requires(!integral<_From>)
STX_HIDE_FROM_ABI constexpr bool __is_index_in_extent(_IndexType __extent, _From __value) {
    if constexpr (is_signed_v<_IndexType>) {
        if (static_cast<_IndexType>(__value) < 0) { return false; }
    }
    return static_cast<_IndexType>(__value) < __extent;
}

template <size_t... _Idxs, class _Extents, class... _From>
STX_HIDE_FROM_ABI constexpr bool __is_multidimensional_index_in_impl(
    index_sequence<_Idxs...>, const _Extents& __ext, _From... __values) {
    return (__is_index_in_extent(__ext.extent(_Idxs), __values) && ...);
}

template <class _Extents, class... _From>
STX_HIDE_FROM_ABI constexpr bool __is_multidimensional_index_in(const _Extents& __ext, _From... __values) {
    return __is_multidimensional_index_in_impl(make_index_sequence<_Extents::rank()>(), __ext, __values...);
}

template <class _Tp> struct __is_extents : false_type {};

template <class _IndexType, size_t... _ExtentsPack>
struct __is_extents<extents<_IndexType, _ExtentsPack...>> : true_type {};

template <class _Tp> inline constexpr bool __is_extents_v = __is_extents<_Tp>::value;

template <class _IndexType, size_t _Rank, class _Extents = extents<_IndexType>> struct __make_dextents;

template <class _IndexType, size_t _Rank, size_t... _ExtentsPack>
struct __make_dextents<_IndexType, _Rank, extents<_IndexType, _ExtentsPack...>> {
    using type STX_NODEBUG =
        typename __make_dextents<_IndexType, _Rank - 1, extents<_IndexType, dynamic_extent, _ExtentsPack...>>::type;
};

template <class _IndexType, size_t... _ExtentsPack>
struct __make_dextents<_IndexType, 0, extents<_IndexType, _ExtentsPack...>> {
    using type STX_NODEBUG = extents<_IndexType, _ExtentsPack...>;
};

template <class _Layout, class _Extents>
concept __has_invalid_mapping = !requires { typename _Layout::template mapping<_Extents>; };

template <class _Layout, class _Mapping>
constexpr bool __is_mapping_of
    = is_same_v<typename _Layout::template mapping<typename _Mapping::extents_type>, _Mapping>;

template <class _Mapping>
concept __layout_mapping_alike = requires {
    requires __is_mapping_of<typename _Mapping::layout_type, _Mapping>;
    requires __is_extents_v<typename _Mapping::extents_type>;
    { _Mapping::is_always_strided() } -> same_as<bool>;
    { _Mapping::is_always_exhaustive() } -> same_as<bool>;
    { _Mapping::is_always_unique() } -> same_as<bool>;
    bool_constant<_Mapping::is_always_strided()>::value;
    bool_constant<_Mapping::is_always_exhaustive()>::value;
    bool_constant<_Mapping::is_always_unique()>::value;
};

} // namespace __mdspan_detail

template <class _IndexType, size_t... _Extents> class extents {
public:
    using index_type = _IndexType;
    using size_type  = make_unsigned_t<index_type>;
    using rank_type  = size_t;

    static_assert(__mdspan_detail::__signed_or_unsigned_integer<index_type>,
        "extents::index_type must be a signed or unsigned integer type");
    static_assert(
        ((__mdspan_detail::__is_representable_as<index_type>(_Extents) || (_Extents == dynamic_extent)) && ...),
        "extents ctor: arguments must be representable as index_type and nonnegative");

private:
    static constexpr rank_type __rank_         = sizeof...(_Extents);
    static constexpr rank_type __rank_dynamic_ = ((_Extents == dynamic_extent) + ... + 0);

    using _Values STX_NODEBUG = __mdspan_detail::__maybe_static_array<_IndexType, size_t, dynamic_extent, _Extents...>;
    STX_NO_UNIQUE_ADDRESS _Values __vals_;

public:
    [[nodiscard]] STX_HIDE_FROM_ABI static constexpr rank_type rank() noexcept { return __rank_; }
    [[nodiscard]] STX_HIDE_FROM_ABI static constexpr rank_type rank_dynamic() noexcept { return __rank_dynamic_; }

    [[nodiscard]] STX_HIDE_FROM_ABI constexpr index_type extent(rank_type __r) const noexcept {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__r < __rank_, "extents::extent: index out of range");
        return __vals_.__value(__r);
    }
    [[nodiscard]] STX_HIDE_FROM_ABI static constexpr size_t static_extent(rank_type __r) noexcept {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__r < __rank_, "extents::static_extent: index out of range");
        return _Values::__static_value(__r);
    }

    STX_HIDE_FROM_ABI constexpr extents() noexcept = default;

    template <class... _OtherIndexTypes>
        requires((is_convertible_v<_OtherIndexTypes, index_type> && ...)
            && (is_nothrow_constructible_v<index_type, _OtherIndexTypes> && ...)
            && (sizeof...(_OtherIndexTypes) == __rank_ || sizeof...(_OtherIndexTypes) == __rank_dynamic_))
    STX_HIDE_FROM_ABI constexpr explicit(sizeof...(_OtherIndexTypes) != __rank_dynamic_)
        extents(_OtherIndexTypes... __dynvals) noexcept
        : __vals_(static_cast<index_type>(__dynvals)...) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__mdspan_detail::__are_representable_as<index_type>(__dynvals...),
            "extents ctor: arguments must be representable as index_type and nonnegative");
    }

    template <class _OtherIndexType, size_t _Size>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
            && is_nothrow_constructible_v<index_type, const _OtherIndexType&>
            && (_Size == __rank_ || _Size == __rank_dynamic_))
    explicit(_Size != __rank_dynamic_) STX_HIDE_FROM_ABI
        constexpr extents(const array<_OtherIndexType, _Size>& __exts) noexcept
        : __vals_(span(__exts)) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__mdspan_detail::__are_representable_as<index_type>(span(__exts)),
            "extents ctor: arguments must be representable as index_type and nonnegative");
    }

    template <class _OtherIndexType, size_t _Size>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
            && is_nothrow_constructible_v<index_type, const _OtherIndexType&>
            && (_Size == __rank_ || _Size == __rank_dynamic_))
    explicit(_Size != __rank_dynamic_) STX_HIDE_FROM_ABI
        constexpr extents(const span<_OtherIndexType, _Size>& __exts) noexcept
        : __vals_(__exts) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__mdspan_detail::__are_representable_as<index_type>(__exts),
            "extents ctor: arguments must be representable as index_type and nonnegative");
    }

private:
    template <size_t _DynCount, size_t _Idx, class _OtherExtents, class... _DynamicValues>
        requires(_Idx < __rank_)
    STX_HIDE_FROM_ABI constexpr _Values __construct_vals_from_extents(integral_constant<size_t, _DynCount>,
        integral_constant<size_t, _Idx>, const _OtherExtents& __exts, _DynamicValues... __dynamic_values) noexcept {
        if constexpr (static_extent(_Idx) == dynamic_extent) {
            return __construct_vals_from_extents(integral_constant<size_t, _DynCount + 1>(),
                integral_constant<size_t, _Idx + 1>(), __exts, __dynamic_values..., __exts.extent(_Idx));
        } else {
            return __construct_vals_from_extents(integral_constant<size_t, _DynCount>(),
                integral_constant<size_t, _Idx + 1>(), __exts, __dynamic_values...);
        }
    }

    template <size_t _DynCount, size_t _Idx, class _OtherExtents, class... _DynamicValues>
        requires((_Idx == __rank_) && (_DynCount == __rank_dynamic_))
    STX_HIDE_FROM_ABI constexpr _Values __construct_vals_from_extents(integral_constant<size_t, _DynCount>,
        integral_constant<size_t, _Idx>, const _OtherExtents&, _DynamicValues... __dynamic_values) noexcept {
        return _Values { static_cast<index_type>(__dynamic_values)... };
    }

public:
    template <class _OtherIndexType, size_t... _OtherExtents>
        requires((sizeof...(_OtherExtents) == sizeof...(_Extents))
            && ((_OtherExtents == dynamic_extent || _Extents == dynamic_extent || _OtherExtents == _Extents) && ...))
    explicit((((_Extents != dynamic_extent) && (_OtherExtents == dynamic_extent)) || ...)
        || (static_cast<make_unsigned_t<index_type>>(numeric_limits<index_type>::max())
            < static_cast<make_unsigned_t<_OtherIndexType>>(numeric_limits<_OtherIndexType>::max()))) STX_HIDE_FROM_ABI
        constexpr extents(const extents<_OtherIndexType, _OtherExtents...>& __other) noexcept
        : __vals_(
              __construct_vals_from_extents(integral_constant<size_t, 0>(), integral_constant<size_t, 0>(), __other)) {
        if constexpr (rank() > 0) {
            for (size_t __r = 0; __r < rank(); __r++) {
                if constexpr (static_cast<make_unsigned_t<index_type>>(numeric_limits<index_type>::max())
                    < static_cast<make_unsigned_t<_OtherIndexType>>(numeric_limits<_OtherIndexType>::max())) {
                    STX_ASSERT_VALID_ELEMENT_ACCESS(
                        __mdspan_detail::__is_representable_as<index_type>(__other.extent(__r)),
                        "extents ctor: arguments must be representable as index_type and nonnegative");
                }
                STX_ASSERT_VALID_ELEMENT_ACCESS((_Values::__static_value(__r) == dynamic_extent)
                        || (static_cast<index_type>(__other.extent(__r))
                            == static_cast<index_type>(_Values::__static_value(__r))),
                    "extents construction: mismatch of provided arguments with static extents.");
            }
        }
    }

    template <class _OtherIndexType, size_t... _OtherExtents>
    STX_HIDE_FROM_ABI friend constexpr bool operator==(
        const extents& __lhs, const extents<_OtherIndexType, _OtherExtents...>& __rhs) noexcept {
        if constexpr (rank() != sizeof...(_OtherExtents)) {
            return false;
        } else {
            for (rank_type __r = 0; __r < __rank_; __r++) {
                using _CommonType = common_type_t<index_type, _OtherIndexType>;
                if (static_cast<_CommonType>(__lhs.extent(__r)) != static_cast<_CommonType>(__rhs.extent(__r))) {
                    return false;
                }
            }
        }
        return true;
    }

    template <class _OtherIndexType>
    STX_HIDE_FROM_ABI static constexpr auto __index_cast(_OtherIndexType&& __i) noexcept {
        using _OtherIndex = remove_cvref_t<_OtherIndexType>;
        if constexpr (integral<_OtherIndex> && !same_as<_OtherIndex, bool>) {
            return __i;
        } else {
            return static_cast<index_type>(std::forward<_OtherIndexType>(__i));
        }
    }
};

template <class _IndexType, size_t _Rank>
using dextents = typename __mdspan_detail::__make_dextents<_IndexType, _Rank>::type;

#if STX_STD_VER >= 26
template <size_t _Rank, class _IndexType = size_t> using dims = dextents<_IndexType, _Rank>;
#endif

#if STX_STD_VER >= 26
template <class... _IndexTypes>
    requires(is_convertible_v<_IndexTypes, size_t> && ...)
explicit extents(_IndexTypes...) -> extents<size_t, __mdspan_detail::__maybe_static_ext<_IndexTypes>...>;
#else
template <class... _IndexTypes>
    requires(is_convertible_v<_IndexTypes, size_t> && ...)
explicit extents(_IndexTypes...) -> extents<size_t, size_t(((void)sizeof(_IndexTypes), dynamic_extent))...>;
#endif

struct layout_left {
    template <class _Extents> class mapping;
};

struct layout_right {
    template <class _Extents> class mapping;
};

struct layout_stride {
    template <class _Extents> class mapping;
};

template <class _Extents> class layout_left::mapping {
public:
    static_assert(__mdspan_detail::__is_extents<_Extents>::value,
        "layout_left::mapping template argument must be a specialization of extents.");

    using extents_type = _Extents;
    using index_type   = extents_type::index_type;
    using size_type    = extents_type::size_type;
    using rank_type    = extents_type::rank_type;
    using layout_type  = layout_left;

private:
    STX_HIDE_FROM_ABI static constexpr bool __required_span_size_is_representable(const extents_type& __ext) {
        if constexpr (extents_type::rank() == 0) { return true; }

        index_type __prod = __ext.extent(0);
        for (rank_type __r = 1; __r < extents_type::rank(); __r++) {
            bool __overflowed = __builtin_mul_overflow(__prod, __ext.extent(__r), std::addressof(__prod));
            if (__overflowed) { return false; }
        }
        return true;
    }

    static_assert(extents_type::rank_dynamic() > 0 || __required_span_size_is_representable(extents_type()),
        "layout_left::mapping product of static extents must be representable as index_type.");

public:
    STX_HIDE_FROM_ABI constexpr mapping() noexcept               = default;
    STX_HIDE_FROM_ABI constexpr mapping(const mapping&) noexcept = default;
    STX_HIDE_FROM_ABI constexpr mapping(const extents_type& __ext) noexcept
        : __extents_(__ext) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__required_span_size_is_representable(__ext),
            "layout_left::mapping extents ctor: product of extents must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents>)
    STX_HIDE_FROM_ABI constexpr explicit(!is_convertible_v<_OtherExtents, extents_type>)
        mapping(const mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_left::mapping converting ctor: other.required_span_size() must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents> && _OtherExtents::rank() <= 1)
    STX_HIDE_FROM_ABI constexpr explicit(!is_convertible_v<_OtherExtents, extents_type>)
        mapping(const layout_right::mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_left::mapping converting ctor: other.required_span_size() must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents>)
    STX_HIDE_FROM_ABI constexpr explicit(extents_type::rank() > 0)
        mapping(const layout_stride::mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        if constexpr (extents_type::rank() > 0) {
            STX_ASSERT_VALID_ELEMENT_ACCESS(([&]() {
                using _CommonType
                    = common_type_t<typename extents_type::index_type, typename _OtherExtents::index_type>;
                for (rank_type __r = 0; __r < extents_type::rank(); __r++) {
                    if (static_cast<_CommonType>(stride(__r)) != static_cast<_CommonType>(__other.stride(__r))) {
                        return false;
                    }
                }
                return true;
            }()),
                "layout_left::mapping from layout_stride ctor: strides are not compatible with layout_left.");
            STX_ASSERT_VALID_ELEMENT_ACCESS(
                __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
                "layout_left::mapping from layout_stride ctor: other.required_span_size() must be representable as "
                "index_type.");
        }
    }

    STX_HIDE_FROM_ABI constexpr mapping& operator=(const mapping&) noexcept = default;
    STX_HIDE_FROM_ABI constexpr const extents_type& extents() const noexcept { return __extents_; }

    STX_HIDE_FROM_ABI constexpr index_type required_span_size() const noexcept {
        index_type __size = 1;
        for (size_t __r = 0; __r < extents_type::rank(); __r++) {
            __size *= __extents_.extent(__r);
        }
        return __size;
    }

    template <class... _Indices>
        requires((sizeof...(_Indices) == extents_type::rank()) && (is_convertible_v<_Indices, index_type> && ...)
            && (is_nothrow_constructible_v<index_type, _Indices> && ...))
    STX_HIDE_FROM_ABI constexpr index_type operator()(_Indices... __idx) const noexcept {
        STX_ASSERT_UNCATEGORIZED(__mdspan_detail::__is_multidimensional_index_in(__extents_, __idx...),
            "layout_left::mapping: out of bounds indexing");
        array<index_type, extents_type::rank()> __idx_a { static_cast<index_type>(__idx)... };
        return [&]<size_t... _Pos>(index_sequence<_Pos...>) {
            index_type __res = 0;
            ((__res = __idx_a[extents_type::rank() - 1 - _Pos]
                     + __extents_.extent(extents_type::rank() - 1 - _Pos) * __res),
                ...);
            return __res;
        }(make_index_sequence<sizeof...(_Indices)>());
    }

    STX_HIDE_FROM_ABI static constexpr bool is_always_unique() noexcept { return true; }
    STX_HIDE_FROM_ABI static constexpr bool is_always_exhaustive() noexcept { return true; }
    STX_HIDE_FROM_ABI static constexpr bool is_always_strided() noexcept { return true; }

    STX_HIDE_FROM_ABI static constexpr bool is_unique() noexcept { return true; }
    STX_HIDE_FROM_ABI static constexpr bool is_exhaustive() noexcept { return true; }
    STX_HIDE_FROM_ABI static constexpr bool is_strided() noexcept { return true; }

    STX_HIDE_FROM_ABI constexpr index_type stride(rank_type __r) const noexcept
        requires(extents_type::rank() > 0)
    {
        STX_ASSERT_VALID_ELEMENT_ACCESS(
            __r < extents_type::rank(), "layout_left::mapping::stride(): invalid rank index");
        index_type __s = 1;
        for (rank_type __i = 0; __i < __r; __i++) {
            __s *= __extents_.extent(__i);
        }
        return __s;
    }

    template <class _OtherExtents>
        requires(_OtherExtents::rank() == extents_type::rank())
    STX_HIDE_FROM_ABI friend constexpr bool operator==(
        const mapping& __lhs, const mapping<_OtherExtents>& __rhs) noexcept {
        return __lhs.extents() == __rhs.extents();
    }

private:
    STX_NO_UNIQUE_ADDRESS extents_type __extents_ {};
};

template <class _Extents> class layout_right::mapping {
public:
    static_assert(__mdspan_detail::__is_extents<_Extents>::value,
        "layout_right::mapping template argument must be a specialization of extents.");

    using extents_type = _Extents;
    using index_type   = extents_type::index_type;
    using size_type    = extents_type::size_type;
    using rank_type    = extents_type::rank_type;
    using layout_type  = layout_right;

private:
    STX_HIDE_FROM_ABI static constexpr bool __required_span_size_is_representable(const extents_type& __ext) {
        if constexpr (extents_type::rank() == 0) { return true; }

        index_type __prod = __ext.extent(0);
        for (rank_type __r = 1; __r < extents_type::rank(); __r++) {
            bool __overflowed = __builtin_mul_overflow(__prod, __ext.extent(__r), std::addressof(__prod));
            if (__overflowed) { return false; }
        }
        return true;
    }

    static_assert(extents_type::rank_dynamic() > 0 || __required_span_size_is_representable(extents_type()),
        "layout_right::mapping product of static extents must be representable as index_type.");

public:
    STX_HIDE_FROM_ABI constexpr mapping() noexcept               = default;
    STX_HIDE_FROM_ABI constexpr mapping(const mapping&) noexcept = default;
    STX_HIDE_FROM_ABI constexpr mapping(const extents_type& __ext) noexcept
        : __extents_(__ext) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__required_span_size_is_representable(__ext),
            "layout_right::mapping extents ctor: product of extents must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents>)
    STX_HIDE_FROM_ABI constexpr explicit(!is_convertible_v<_OtherExtents, extents_type>)
        mapping(const mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_right::mapping converting ctor: other.required_span_size() must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents> && _OtherExtents::rank() <= 1)
    STX_HIDE_FROM_ABI constexpr explicit(!is_convertible_v<_OtherExtents, extents_type>)
        mapping(const layout_left::mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_right::mapping converting ctor: other.required_span_size() must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents>)
    STX_HIDE_FROM_ABI constexpr explicit(extents_type::rank() > 0)
        mapping(const layout_stride::mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        if constexpr (extents_type::rank() > 0) {
            STX_ASSERT_VALID_ELEMENT_ACCESS(([&]() {
                using _CommonType
                    = common_type_t<typename extents_type::index_type, typename _OtherExtents::index_type>;
                for (rank_type __r = 0; __r < extents_type::rank(); __r++) {
                    if (static_cast<_CommonType>(stride(__r)) != static_cast<_CommonType>(__other.stride(__r))) {
                        return false;
                    }
                }
                return true;
            }()),
                "layout_right::mapping from layout_stride ctor: strides are not compatible with layout_right.");
            STX_ASSERT_VALID_ELEMENT_ACCESS(
                __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
                "layout_right::mapping from layout_stride ctor: other.required_span_size() must be representable as "
                "index_type.");
        }
    }

    STX_HIDE_FROM_ABI constexpr mapping& operator=(const mapping&) noexcept = default;
    STX_HIDE_FROM_ABI constexpr const extents_type& extents() const noexcept { return __extents_; }

    STX_HIDE_FROM_ABI constexpr index_type required_span_size() const noexcept {
        index_type __size = 1;
        for (size_t __r = 0; __r < extents_type::rank(); __r++) {
            __size *= __extents_.extent(__r);
        }
        return __size;
    }

    template <class... _Indices>
        requires((sizeof...(_Indices) == extents_type::rank()) && (is_convertible_v<_Indices, index_type> && ...)
            && (is_nothrow_constructible_v<index_type, _Indices> && ...))
    STX_HIDE_FROM_ABI constexpr index_type operator()(_Indices... __idx) const noexcept {
        STX_ASSERT_UNCATEGORIZED(__mdspan_detail::__is_multidimensional_index_in(__extents_, __idx...),
            "layout_right::mapping: out of bounds indexing");
        return [&]<size_t... _Pos>(index_sequence<_Pos...>) {
            index_type __res = 0;
            ((__res = static_cast<index_type>(__idx) + __extents_.extent(_Pos) * __res), ...);
            return __res;
        }(make_index_sequence<sizeof...(_Indices)>());
    }

    STX_HIDE_FROM_ABI static constexpr bool is_always_unique() noexcept { return true; }
    STX_HIDE_FROM_ABI static constexpr bool is_always_exhaustive() noexcept { return true; }
    STX_HIDE_FROM_ABI static constexpr bool is_always_strided() noexcept { return true; }

    STX_HIDE_FROM_ABI static constexpr bool is_unique() noexcept { return true; }
    STX_HIDE_FROM_ABI static constexpr bool is_exhaustive() noexcept { return true; }
    STX_HIDE_FROM_ABI static constexpr bool is_strided() noexcept { return true; }

    STX_HIDE_FROM_ABI constexpr index_type stride(rank_type __r) const noexcept
        requires(extents_type::rank() > 0)
    {
        STX_ASSERT_VALID_ELEMENT_ACCESS(
            __r < extents_type::rank(), "layout_right::mapping::stride(): invalid rank index");
        index_type __s = 1;
        for (rank_type __i = extents_type::rank() - 1; __i > __r; __i--) {
            __s *= __extents_.extent(__i);
        }
        return __s;
    }

    template <class _OtherExtents>
        requires(_OtherExtents::rank() == extents_type::rank())
    STX_HIDE_FROM_ABI friend constexpr bool operator==(
        const mapping& __lhs, const mapping<_OtherExtents>& __rhs) noexcept {
        return __lhs.extents() == __rhs.extents();
    }

private:
    STX_NO_UNIQUE_ADDRESS extents_type __extents_ {};
};

template <class _Extents> class layout_stride::mapping {
public:
    static_assert(__mdspan_detail::__is_extents<_Extents>::value,
        "layout_stride::mapping template argument must be a specialization of extents.");

    using extents_type = _Extents;
    using index_type   = extents_type::index_type;
    using size_type    = extents_type::size_type;
    using rank_type    = extents_type::rank_type;
    using layout_type  = layout_stride;

private:
    static constexpr rank_type __rank_ = extents_type::rank();

    STX_HIDE_FROM_ABI static constexpr bool __required_span_size_is_representable(const extents_type& __ext) {
        if constexpr (__rank_ == 0) { return true; }

        index_type __prod = __ext.extent(0);
        for (rank_type __r = 1; __r < __rank_; __r++) {
            bool __overflowed = __builtin_mul_overflow(__prod, __ext.extent(__r), std::addressof(__prod));
            if (__overflowed) { return false; }
        }
        return true;
    }

    template <class _OtherIndexType>
    STX_HIDE_FROM_ABI static constexpr bool __required_span_size_is_representable(
        const extents_type& __ext, span<_OtherIndexType, __rank_> __strides) {
        if constexpr (__rank_ == 0) { return true; }

        index_type __size = 1;
        for (rank_type __r = 0; __r < __rank_; __r++) {
            if constexpr (is_integral_v<_OtherIndexType>) {
                using _CommonType = common_type_t<index_type, _OtherIndexType>;
                if (static_cast<_CommonType>(__strides[__r])
                    > static_cast<_CommonType>(numeric_limits<index_type>::max())) {
                    return false;
                }
            }
            if (__ext.extent(__r) == static_cast<index_type>(0)) { return true; }
            index_type __prod = (__ext.extent(__r) - 1);
            bool __overflowed_mul
                = __builtin_mul_overflow(__prod, static_cast<index_type>(__strides[__r]), std::addressof(__prod));
            if (__overflowed_mul) { return false; }
            bool __overflowed_add = __builtin_add_overflow(__size, __prod, std::addressof(__size));
            if (__overflowed_add) { return false; }
        }
        return true;
    }

    template <class _StridedMapping>
    STX_HIDE_FROM_ABI static constexpr index_type __offset(const _StridedMapping& __mapping) {
        if constexpr (_StridedMapping::extents_type::rank() == 0) {
            return static_cast<index_type>(__mapping());
        } else if (__mapping.required_span_size() == static_cast<typename _StridedMapping::index_type>(0)) {
            return static_cast<index_type>(0);
        } else {
            return [&]<size_t... _Pos>(index_sequence<_Pos...>) {
                return static_cast<index_type>(__mapping((_Pos ? 0 : 0)...));
            }(make_index_sequence<__rank_>());
        }
    }

    STX_HIDE_FROM_ABI constexpr void __bubble_sort_by_strides(array<rank_type, __rank_>& __permute) const {
        for (rank_type __i = __rank_ - 1; __i > 0; __i--) {
            for (rank_type __r = 0; __r < __i; __r++) {
                if (__strides_[__permute[__r]] > __strides_[__permute[__r + 1]]) {
                    swap(__permute[__r], __permute[__r + 1]);
                } else {
                    if ((__strides_[__permute[__r]] == __strides_[__permute[__r + 1]])
                        && (__extents_.extent(__permute[__r]) > static_cast<index_type>(1))) {
                        swap(__permute[__r], __permute[__r + 1]);
                    }
                }
            }
        }
    }

    static_assert(extents_type::rank_dynamic() > 0 || __required_span_size_is_representable(extents_type()),
        "layout_stride::mapping product of static extents must be representable as index_type.");

public:
    STX_HIDE_FROM_ABI constexpr mapping() noexcept
        : __extents_(extents_type()) {
        if constexpr (__rank_ > 0) {
            index_type __stride = 1;
            for (rank_type __r = __rank_ - 1; __r > static_cast<rank_type>(0); __r--) {
                __strides_[__r] = __stride;
                __stride *= __extents_.extent(__r);
            }
            __strides_[0] = __stride;
        }
    }

    STX_HIDE_FROM_ABI constexpr mapping(const mapping&) noexcept = default;

    template <class _OtherIndexType>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
                    && is_nothrow_constructible_v<index_type, const _OtherIndexType&>)
    STX_HIDE_FROM_ABI constexpr mapping(const extents_type& __ext, span<_OtherIndexType, __rank_> __strides) noexcept
        : __extents_(__ext)
        , __strides_([&]<size_t... _Pos>(index_sequence<_Pos...>) {
            return __mdspan_detail::__possibly_empty_array<index_type, __rank_> { static_cast<index_type>(
                std::as_const(__strides[_Pos]))... };
        }(make_index_sequence<__rank_>())) {
        STX_ASSERT_VALID_ELEMENT_ACCESS(([&]<size_t... _Pos>(index_sequence<_Pos...>) {
            if constexpr (is_integral_v<_OtherIndexType>) {
                return ((__strides[_Pos] > static_cast<_OtherIndexType>(0)) && ... && true);
            } else {
                return ((static_cast<index_type>(__strides[_Pos]) > static_cast<index_type>(0)) && ... && true);
            }
        }(make_index_sequence<__rank_>())),
            "layout_stride::mapping ctor: all strides must be greater than 0");
        STX_ASSERT_VALID_ELEMENT_ACCESS(__required_span_size_is_representable(__ext, __strides),
            "layout_stride::mapping ctor: required span size is not representable as index_type.");
        if constexpr (__rank_ > 1) {
            STX_ASSERT_UNCATEGORIZED(([&]<size_t... _Pos>(index_sequence<_Pos...>) {
                array<rank_type, __rank_> __permute { _Pos... };
                __bubble_sort_by_strides(__permute);

                for (rank_type __i = 1; __i < __rank_; __i++) {
                    if (static_cast<index_type>(__strides[__permute[__i]])
                        < static_cast<index_type>(__strides[__permute[__i - 1]])
                            * __extents_.extent(__permute[__i - 1])) {
                        return false;
                    }
                }
                return true;
            }(make_index_sequence<__rank_>())),
                "layout_stride::mapping ctor: the provided extents and strides lead to a non-unique mapping");
        }
    }

    template <class _OtherIndexType>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
            && is_nothrow_constructible_v<index_type, const _OtherIndexType&>)
    STX_HIDE_FROM_ABI constexpr mapping(
        const extents_type& __ext, const array<_OtherIndexType, __rank_>& __strides) noexcept
        : mapping(__ext, span(__strides)) {}

    template <class _StridedLayoutMapping>
        requires(__mdspan_detail::__layout_mapping_alike<_StridedLayoutMapping>
                    && is_constructible_v<extents_type, typename _StridedLayoutMapping::extents_type>
                    && _StridedLayoutMapping::is_always_unique() && _StridedLayoutMapping::is_always_strided())
    STX_HIDE_FROM_ABI constexpr explicit(!(is_convertible_v<typename _StridedLayoutMapping::extents_type, extents_type>
        && (__mdspan_detail::__is_mapping_of<layout_left, _StridedLayoutMapping>
            || __mdspan_detail::__is_mapping_of<layout_right, _StridedLayoutMapping>
            || __mdspan_detail::__is_mapping_of<layout_stride, _StridedLayoutMapping>)))
        mapping(const _StridedLayoutMapping& __other) noexcept
        : __extents_(__other.extents())
        , __strides_([&]<size_t... _Pos>(index_sequence<_Pos...>) {
            if constexpr (__rank_ > 0) {
                return __mdspan_detail::__possibly_empty_array<index_type, __rank_> { static_cast<index_type>(
                    __other.stride(_Pos))... };
            } else {
                return __mdspan_detail::__possibly_empty_array<index_type, 0> {};
            }
        }(make_index_sequence<__rank_>())) {
        if constexpr (__rank_ > 0) {
            STX_ASSERT_VALID_ELEMENT_ACCESS(([&]<size_t... _Pos>(index_sequence<_Pos...>) {
                return ((static_cast<index_type>(__other.stride(_Pos)) > static_cast<index_type>(0)) && ... && true);
            }(make_index_sequence<__rank_>())),
                "layout_stride::mapping converting ctor: all strides must be greater than 0");
        }
        STX_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_stride::mapping converting ctor: other.required_span_size() must be representable as index_type.");
        STX_ASSERT_VALID_ELEMENT_ACCESS(static_cast<index_type>(0) == __offset(__other),
            "layout_stride::mapping converting ctor: base offset of mapping must be zero.");
    }

    STX_HIDE_FROM_ABI constexpr mapping& operator=(const mapping&) noexcept = default;
    STX_HIDE_FROM_ABI constexpr const extents_type& extents() const noexcept { return __extents_; }

    STX_HIDE_FROM_ABI constexpr array<index_type, __rank_> strides() const noexcept {
        return [&]<size_t... _Pos>(index_sequence<_Pos...>) {
            return array<index_type, __rank_> { __strides_[_Pos]... };
        }(make_index_sequence<__rank_>());
    }

    STX_HIDE_FROM_ABI constexpr index_type required_span_size() const noexcept {
        if constexpr (__rank_ == 0) {
            return static_cast<index_type>(1);
        } else {
            return [&]<size_t... _Pos>(index_sequence<_Pos...>) {
                if ((__extents_.extent(_Pos) * ... * 1) == 0) { return static_cast<index_type>(0); }

                return static_cast<index_type>(static_cast<index_type>(1)
                    + (((__extents_.extent(_Pos) - static_cast<index_type>(1)) * __strides_[_Pos]) + ...
                        + static_cast<index_type>(0)));
            }(make_index_sequence<__rank_>());
        }
    }

    template <class... _Indices>
        requires((sizeof...(_Indices) == __rank_) && (is_convertible_v<_Indices, index_type> && ...)
            && (is_nothrow_constructible_v<index_type, _Indices> && ...))
    STX_HIDE_FROM_ABI constexpr index_type operator()(_Indices... __idx) const noexcept {
        STX_ASSERT_UNCATEGORIZED(__mdspan_detail::__is_multidimensional_index_in(__extents_, __idx...),
            "layout_stride::mapping: out of bounds indexing");
        return [&]<size_t... _Pos>(index_sequence<_Pos...>) {
            return ((static_cast<index_type>(__idx) * __strides_[_Pos]) + ... + index_type(0));
        }(make_index_sequence<sizeof...(_Indices)>());
    }

    STX_HIDE_FROM_ABI static constexpr bool is_always_unique() noexcept { return true; }
    STX_HIDE_FROM_ABI static constexpr bool is_always_exhaustive() noexcept { return false; }
    STX_HIDE_FROM_ABI static constexpr bool is_always_strided() noexcept { return true; }

    STX_HIDE_FROM_ABI static constexpr bool is_unique() noexcept { return true; }
    STX_HIDE_FROM_ABI constexpr bool is_exhaustive() const noexcept {
        if constexpr (__rank_ == 0) {
            return true;
        } else {
            index_type __span_size = required_span_size();
            if (__span_size == static_cast<index_type>(0)) {
                if constexpr (__rank_ == 1) {
                    return __strides_[0] == 1;
                } else {
                    rank_type __r_largest = 0;
                    for (rank_type __r = 1; __r < __rank_; __r++) {
                        if (__strides_[__r] > __strides_[__r_largest]) { __r_largest = __r; }
                    }
                    for (rank_type __r = 0; __r < __rank_; __r++) {
                        if (__extents_.extent(__r) == 0 && __r != __r_largest) { return false; }
                    }
                    return true;
                }
            } else {
                return required_span_size() == [&]<size_t... _Pos>(index_sequence<_Pos...>) {
                    return (__extents_.extent(_Pos) * ... * static_cast<index_type>(1));
                }(make_index_sequence<__rank_>());
            }
        }
    }
    STX_HIDE_FROM_ABI static constexpr bool is_strided() noexcept { return true; }

    STX_HIDE_FROM_ABI constexpr index_type stride(rank_type __r) const noexcept {
        STX_ASSERT_VALID_ELEMENT_ACCESS(__r < __rank_, "layout_stride::mapping::stride(): invalid rank index");
        return __strides_[__r];
    }

    template <class _OtherMapping>
        requires(__mdspan_detail::__layout_mapping_alike<_OtherMapping>
            && (_OtherMapping::extents_type::rank() == __rank_) && _OtherMapping::is_always_strided())
    STX_HIDE_FROM_ABI friend constexpr bool operator==(const mapping& __lhs, const _OtherMapping& __rhs) noexcept {
        if (__offset(__rhs)) { return false; }
        if constexpr (__rank_ == 0) {
            return true;
        } else {
            return __lhs.extents() == __rhs.extents() && [&]<size_t... _Pos>(index_sequence<_Pos...>) {
                using _CommonType = common_type_t<index_type, typename _OtherMapping::index_type>;
                return ((static_cast<_CommonType>(__lhs.stride(_Pos)) == static_cast<_CommonType>(__rhs.stride(_Pos)))
                    && ... && true);
            }(make_index_sequence<__rank_>());
        }
    }

private:
    STX_NO_UNIQUE_ADDRESS extents_type __extents_ {};
    STX_NO_UNIQUE_ADDRESS __mdspan_detail::__possibly_empty_array<index_type, __rank_> __strides_ {};
};

template <class _ElementType> class default_accessor {
public:
    static_assert(!is_array_v<_ElementType>, "default_accessor: template argument may not be an array type");
    static_assert(!is_abstract_v<_ElementType>, "default_accessor: template argument may not be an abstract class");
    using offset_policy    = default_accessor;
    using element_type     = _ElementType;
    using reference        = _ElementType&;
    using data_handle_type = _ElementType*;

    STX_HIDE_FROM_ABI constexpr default_accessor() noexcept = default;

    template <class _OtherElementType>
        requires(is_convertible_v<_OtherElementType (*)[], element_type (*)[]>)
    STX_HIDE_FROM_ABI constexpr default_accessor(default_accessor<_OtherElementType>) noexcept {}

    STX_HIDE_FROM_ABI constexpr reference access(data_handle_type __p, size_t __i) const noexcept { return __p[__i]; }
    STX_HIDE_FROM_ABI constexpr data_handle_type offset(data_handle_type __p, size_t __i) const noexcept {
        return __p + __i;
    }
};

template <class _ElementType, class _Extents, class _LayoutPolicy = layout_right,
    class _AccessorPolicy = default_accessor<_ElementType>>
class mdspan {
private:
    static_assert(__mdspan_detail::__is_extents_v<_Extents>,
        "mdspan: Extents template parameter must be a specialization of extents.");
    static_assert(
        is_object_v<_ElementType> && requires { sizeof(_ElementType); },
        "mdspan: ElementType template parameter must be a complete object type");
    static_assert(!is_array_v<_ElementType>, "mdspan: ElementType template parameter may not be an array type");
    static_assert(!is_abstract_v<_ElementType>, "mdspan: ElementType template parameter may not be an abstract class");
    static_assert(is_same_v<_ElementType, typename _AccessorPolicy::element_type>,
        "mdspan: ElementType template parameter must match AccessorPolicy::element_type");
    static_assert(!__mdspan_detail::__has_invalid_mapping<_LayoutPolicy, _Extents>,
        "mdspan: LayoutPolicy template parameter is invalid. A common mistake is to pass a layout mapping "
        "instead of a layout policy");

public:
    using extents_type     = _Extents;
    using layout_type      = _LayoutPolicy;
    using accessor_type    = _AccessorPolicy;
    using mapping_type     = typename layout_type::template mapping<extents_type>;
    using element_type     = _ElementType;
    using value_type       = remove_cv_t<element_type>;
    using index_type       = typename extents_type::index_type;
    using size_type        = typename extents_type::size_type;
    using rank_type        = typename extents_type::rank_type;
    using data_handle_type = typename accessor_type::data_handle_type;
    using reference        = typename accessor_type::reference;

    [[nodiscard]] STX_HIDE_FROM_ABI static constexpr rank_type rank() noexcept { return extents_type::rank(); }
    [[nodiscard]] STX_HIDE_FROM_ABI static constexpr rank_type rank_dynamic() noexcept {
        return extents_type::rank_dynamic();
    }
    [[nodiscard]] STX_HIDE_FROM_ABI static constexpr size_t static_extent(rank_type __r) noexcept {
        return extents_type::static_extent(__r);
    }
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr index_type extent(rank_type __r) const noexcept {
        return __map_.extents().extent(__r);
    }

    STX_HIDE_FROM_ABI constexpr mdspan()
        requires((extents_type::rank_dynamic() > 0) && is_default_constructible_v<data_handle_type>
                    && is_default_constructible_v<mapping_type> && is_default_constructible_v<accessor_type>)
    = default;
    STX_HIDE_FROM_ABI constexpr mdspan(const mdspan&) = default;
    STX_HIDE_FROM_ABI constexpr mdspan(mdspan&&)      = default;

    template <class... _OtherIndexTypes>
        requires((is_convertible_v<_OtherIndexTypes, index_type> && ...)
                    && (is_nothrow_constructible_v<index_type, _OtherIndexTypes> && ...)
                    && ((sizeof...(_OtherIndexTypes) == rank()) || (sizeof...(_OtherIndexTypes) == rank_dynamic()))
                    && is_constructible_v<mapping_type, extents_type> && is_default_constructible_v<accessor_type>)
    STX_HIDE_FROM_ABI explicit constexpr mdspan(data_handle_type __p, _OtherIndexTypes... __exts)
        : __ptr_(std::move(__p))
        , __map_(extents_type(static_cast<index_type>(std::move(__exts))...))
        , __acc_ {} {}

    template <class _OtherIndexType, size_t _Size>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
                    && is_nothrow_constructible_v<index_type, const _OtherIndexType&>
                    && ((_Size == rank()) || (_Size == rank_dynamic()))
                    && is_constructible_v<mapping_type, extents_type> && is_default_constructible_v<accessor_type>)
    explicit(_Size != rank_dynamic()) STX_HIDE_FROM_ABI
        constexpr mdspan(data_handle_type __p, const array<_OtherIndexType, _Size>& __exts)
        : __ptr_(std::move(__p))
        , __map_(extents_type(__exts))
        , __acc_ {} {}

    template <class _OtherIndexType, size_t _Size>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
                    && is_nothrow_constructible_v<index_type, const _OtherIndexType&>
                    && ((_Size == rank()) || (_Size == rank_dynamic()))
                    && is_constructible_v<mapping_type, extents_type> && is_default_constructible_v<accessor_type>)
    explicit(_Size != rank_dynamic()) STX_HIDE_FROM_ABI
        constexpr mdspan(data_handle_type __p, span<_OtherIndexType, _Size> __exts)
        : __ptr_(std::move(__p))
        , __map_(extents_type(__exts))
        , __acc_ {} {}

    STX_HIDE_FROM_ABI constexpr mdspan(data_handle_type __p, const extents_type& __exts)
        requires(is_default_constructible_v<accessor_type> && is_constructible_v<mapping_type, const extents_type&>)
        : __ptr_(std::move(__p))
        , __map_(__exts)
        , __acc_ {} {}

    STX_HIDE_FROM_ABI constexpr mdspan(data_handle_type __p, const extents_type& __exts, const accessor_type& __a)
        requires(is_constructible_v<mapping_type, const extents_type&>)
        : __ptr_(std::move(__p))
        , __map_(__exts)
        , __acc_(__a) {}

    STX_HIDE_FROM_ABI constexpr mdspan(data_handle_type __p, const mapping_type& __m)
        requires(is_default_constructible_v<accessor_type>)
        : __ptr_(std::move(__p))
        , __map_(__m)
        , __acc_ {} {}

    STX_HIDE_FROM_ABI constexpr mdspan(data_handle_type __p, const mapping_type& __m, const accessor_type& __a)
        : __ptr_(std::move(__p))
        , __map_(__m)
        , __acc_(__a) {}

    template <class _OtherElementType, class _OtherExtents, class _OtherLayoutPolicy, class _OtherAccessor>
        requires(is_constructible_v<mapping_type, const typename _OtherLayoutPolicy::template mapping<_OtherExtents>&>
                    && is_constructible_v<accessor_type, const _OtherAccessor&>
                    && is_constructible_v<data_handle_type, const typename _OtherAccessor::data_handle_type&>
                    && is_constructible_v<extents_type, _OtherExtents>)
    explicit(!is_convertible_v<const typename _OtherLayoutPolicy::template mapping<_OtherExtents>&, mapping_type>
        || !is_convertible_v<const _OtherAccessor&, accessor_type>) STX_HIDE_FROM_ABI
        constexpr mdspan(const mdspan<_OtherElementType, _OtherExtents, _OtherLayoutPolicy, _OtherAccessor>& __other)
        : __ptr_(__other.__ptr_)
        , __map_(__other.__map_)
        , __acc_(__other.__acc_) {
        if constexpr (rank() > 0) {
            for (size_t __r = 0; __r < rank(); __r++) {
                STX_ASSERT_VALID_ELEMENT_ACCESS((static_extent(__r) == dynamic_extent)
                        || (static_cast<index_type>(__other.extent(__r))
                            == static_cast<index_type>(static_extent(__r))),
                    "mdspan: conversion mismatch of source dynamic extents with static extents");
            }
        }
    }

    STX_HIDE_FROM_ABI constexpr mdspan& operator=(const mdspan&) = default;
    STX_HIDE_FROM_ABI constexpr mdspan& operator=(mdspan&&)      = default;

    template <class... _OtherIndexTypes>
        requires((is_convertible_v<_OtherIndexTypes, index_type> && ...)
            && (is_nothrow_constructible_v<index_type, _OtherIndexTypes> && ...)
            && (sizeof...(_OtherIndexTypes) == rank()))
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr reference operator[](_OtherIndexTypes... __indices) const {
        return [&]<class... _IndexTypes>(_IndexTypes... __idxs) -> reference {
            STX_ASSERT_VALID_ELEMENT_ACCESS(__mdspan_detail::__is_multidimensional_index_in(extents(), __idxs...),
                "mdspan: operator[] out of bounds access");
            return __acc_.access(__ptr_, __map_(static_cast<index_type>(std::move(__idxs))...));
        }(extents_type::__index_cast(std::move(__indices))...);
    }

    template <class _OtherIndexType>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
            && is_nothrow_constructible_v<index_type, const _OtherIndexType&>)
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr reference operator[](
        const array<_OtherIndexType, rank()>& __indices) const {
        return __acc_.access(__ptr_, [&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            return __map_(extents_type::__index_cast(__indices[_Idxs])...);
        }(make_index_sequence<rank()>()));
    }

    template <class _OtherIndexType>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
            && is_nothrow_constructible_v<index_type, const _OtherIndexType&>)
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr reference operator[](span<_OtherIndexType, rank()> __indices) const {
        return __acc_.access(__ptr_, [&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            return __map_(extents_type::__index_cast(__indices[_Idxs])...);
        }(make_index_sequence<rank()>()));
    }

    [[nodiscard]] STX_HIDE_FROM_ABI constexpr size_type size() const noexcept {
        STX_ASSERT_UNCATEGORIZED(false == ([&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            size_type __prod = 1;
            return (__builtin_mul_overflow(__prod, extent(_Idxs), std::addressof(__prod)) || ... || false);
        }(make_index_sequence<rank()>())),
            "mdspan: size() is not representable as size_type");
        return [&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            return ((static_cast<size_type>(__map_.extents().extent(_Idxs))) * ... * size_type(1));
        }(make_index_sequence<rank()>());
    }

    [[nodiscard]] STX_HIDE_FROM_ABI constexpr bool empty() const noexcept {
        return [&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            return (rank() > 0) && ((__map_.extents().extent(_Idxs) == index_type(0)) || ... || false);
        }(make_index_sequence<rank()>());
    }

    STX_HIDE_FROM_ABI friend constexpr void swap(mdspan& __x, mdspan& __y) noexcept {
        swap(__x.__ptr_, __y.__ptr_);
        swap(__x.__map_, __y.__map_);
        swap(__x.__acc_, __y.__acc_);
    }

    [[nodiscard]] STX_HIDE_FROM_ABI constexpr const extents_type& extents() const noexcept { return __map_.extents(); }
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr const data_handle_type& data_handle() const noexcept { return __ptr_; }
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr const mapping_type& mapping() const noexcept { return __map_; }
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr const accessor_type& accessor() const noexcept { return __acc_; }

    [[nodiscard]] STX_HIDE_FROM_ABI static constexpr bool is_always_unique() noexcept {
        return mapping_type::is_always_unique();
    }
    [[nodiscard]] STX_HIDE_FROM_ABI static constexpr bool is_always_exhaustive() noexcept {
        return mapping_type::is_always_exhaustive();
    }
    [[nodiscard]] STX_HIDE_FROM_ABI static constexpr bool is_always_strided() noexcept {
        return mapping_type::is_always_strided();
    }

    [[nodiscard]] STX_HIDE_FROM_ABI constexpr bool is_unique() const { return __map_.is_unique(); }
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr bool is_exhaustive() const { return __map_.is_exhaustive(); }
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr bool is_strided() const { return __map_.is_strided(); }
    [[nodiscard]] STX_HIDE_FROM_ABI constexpr index_type stride(rank_type __r) const { return __map_.stride(__r); }

private:
    STX_NO_UNIQUE_ADDRESS data_handle_type __ptr_ {};
    STX_NO_UNIQUE_ADDRESS mapping_type __map_ {};
    STX_NO_UNIQUE_ADDRESS accessor_type __acc_ {};

    template <class, class, class, class> friend class mdspan;
};

#if STX_STD_VER >= 26
template <class _ElementType, class... _OtherIndexTypes>
    requires((is_convertible_v<_OtherIndexTypes, size_t> && ...) && (sizeof...(_OtherIndexTypes) > 0))
explicit mdspan(_ElementType*, _OtherIndexTypes...)
    -> mdspan<_ElementType, extents<size_t, __mdspan_detail::__maybe_static_ext<_OtherIndexTypes>...>>;
#else
template <class _ElementType, class... _OtherIndexTypes>
    requires((is_convertible_v<_OtherIndexTypes, size_t> && ...) && (sizeof...(_OtherIndexTypes) > 0))
explicit mdspan(_ElementType*, _OtherIndexTypes...)
    -> mdspan<_ElementType, dextents<size_t, sizeof...(_OtherIndexTypes)>>;
#endif

template <class _Pointer>
    requires(is_pointer_v<remove_reference_t<_Pointer>>)
mdspan(_Pointer&&) -> mdspan<remove_pointer_t<remove_reference_t<_Pointer>>, extents<size_t>>;

template <class _CArray>
    requires(is_array_v<_CArray> && (rank_v<_CArray> == 1))
mdspan(_CArray&) -> mdspan<remove_all_extents_t<_CArray>, extents<size_t, extent_v<_CArray, 0>>>;

template <class _ElementType, class _OtherIndexType, size_t _Size>
mdspan(_ElementType*, const array<_OtherIndexType, _Size>&) -> mdspan<_ElementType, dextents<size_t, _Size>>;

template <class _ElementType, class _OtherIndexType, size_t _Size>
mdspan(_ElementType*, span<_OtherIndexType, _Size>) -> mdspan<_ElementType, dextents<size_t, _Size>>;

template <class _ElementType, class _OtherIndexType, size_t... _ExtentsPack>
mdspan(_ElementType*, const extents<_OtherIndexType, _ExtentsPack...>&)
    -> mdspan<_ElementType, extents<_OtherIndexType, _ExtentsPack...>>;

template <class _ElementType, class _MappingType>
mdspan(_ElementType*, const _MappingType&)
    -> mdspan<_ElementType, typename _MappingType::extents_type, typename _MappingType::layout_type>;

template <class _MappingType, class _AccessorType>
mdspan(typename _AccessorType::data_handle_type, const _MappingType&, const _AccessorType&)
    -> mdspan<typename _AccessorType::element_type, typename _MappingType::extents_type,
        typename _MappingType::layout_type, _AccessorType>;

#endif // STX_STD_VER >= 23

} // namespace stx

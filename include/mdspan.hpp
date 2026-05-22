/*
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *                        Kokkos v. 4.0
 *       Copyright (2022) National Technology & Engineering
 *               Solutions of Sandia, LLC (NTESS).
 *
 * Under the terms of Contract DE-NA0003525 with NTESS,
 * the U.S. Government retains certain rights in this software.
 */

#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

// --- Compatibility layer for LLVM libc++ specific macros ---
#ifndef _LIBCPP_BEGIN_NAMESPACE_STD
#define _LIBCPP_BEGIN_NAMESPACE_STD \
    namespace stx { \
    using namespace std;
#define _STX_SHIM_BEGIN_NAMESPACE_STD
#endif
#ifndef _LIBCPP_END_NAMESPACE_STD
#define _LIBCPP_END_NAMESPACE_STD }
#define _STX_SHIM_END_NAMESPACE_STD
#endif
#ifndef _LIBCPP_HIDE_FROM_ABI
#define _LIBCPP_HIDE_FROM_ABI inline
#define _STX_SHIM_HIDE_FROM_ABI
#endif
#ifndef _LIBCPP_NO_UNIQUE_ADDRESS
#define _LIBCPP_NO_UNIQUE_ADDRESS [[no_unique_address]]
#define _STX_SHIM_NO_UNIQUE_ADDRESS
#endif
#ifndef _LIBCPP_NODEBUG
#define _LIBCPP_NODEBUG
#define _STX_SHIM_NODEBUG
#endif
#ifndef _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS
#define _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(cond, msg) assert((cond) && msg)
#define _STX_SHIM_ASSERT_VALID_ELEMENT_ACCESS
#endif
#ifndef _LIBCPP_ASSERT_UNCATEGORIZED
#define _LIBCPP_ASSERT_UNCATEGORIZED(cond, msg) assert((cond) && msg)
#define _STX_SHIM_ASSERT_UNCATEGORIZED
#endif
#ifndef _LIBCPP_PUSH_MACROS
#define _LIBCPP_PUSH_MACROS
#define _STX_SHIM_PUSH_MACROS
#endif
#ifndef _LIBCPP_POP_MACROS
#define _LIBCPP_POP_MACROS
#define _STX_SHIM_POP_MACROS
#endif
#ifndef _LIBCPP_STD_VER
#if __cplusplus >= 202600L
#define _LIBCPP_STD_VER 26
#else
#define _LIBCPP_STD_VER 26 // Force C++26 features for standalone compatibility
#endif
#define _STX_SHIM_STD_VER
#endif

#ifndef __libcpp_unreachable
#define __libcpp_unreachable() __builtin_unreachable()
#define _STX_SHIM_UNREACHABLE
#endif

// Define C++26 draft concept and helper templates used by libc++ mdspan deduction guides,
// as well as custom emulation of libc++ internal integer concepts required by mdspan.
namespace stx {
using namespace std;

template <class _Tp> inline constexpr bool __is_character_v = false;
template <> inline constexpr bool __is_character_v<char>    = true;
template <> inline constexpr bool __is_character_v<wchar_t> = true;
#ifdef __cpp_char8_t
template <> inline constexpr bool __is_character_v<char8_t> = true;
#endif
template <> inline constexpr bool __is_character_v<char16_t> = true;
template <> inline constexpr bool __is_character_v<char32_t> = true;

template <class _Tp>
concept __signed_or_unsigned_integer = integral<_Tp> && !same_as<_Tp, bool> && !__is_character_v<_Tp>;

template <class _Tp>
concept __integral_constant_like = is_integral_v<remove_cvref_t<decltype(_Tp::value)>>
    && !is_same_v<bool, remove_cvref_t<decltype(_Tp::value)>> && convertible_to<_Tp, decltype(_Tp::value)>
    && equality_comparable_with<_Tp, decltype(_Tp::value)> && bool_constant<_Tp() == _Tp::value>::value
    && bool_constant<static_cast<decltype(_Tp::value)>(_Tp()) == _Tp::value>::value;

template <class _Tp> inline constexpr size_t __maybe_static_ext = dynamic_extent;

template <__integral_constant_like _Tp> inline constexpr size_t __maybe_static_ext<_Tp> = { _Tp::value };
} // namespace stx

// =============================================================================
// FILE: include/__fwd/mdspan.h
// =============================================================================
// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
//===---------------------------------------------------------------------===//

#ifndef _LIBCPP___MDSPAN_LAYOUTS_H
#define _LIBCPP___MDSPAN_LAYOUTS_H

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#pragma GCC system_header
#endif

_LIBCPP_PUSH_MACROS

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER >= 23

// Layout policy with a mapping which corresponds to FORTRAN-style array layouts
struct layout_left {
    template <class _Extents> class mapping;
};

// Layout policy with a mapping which corresponds to C-style array layouts
struct layout_right {
    template <class _Extents> class mapping;
};

// Layout policy with a unique mapping where strides are arbitrary
struct layout_stride {
    template <class _Extents> class mapping;
};

#endif // _LIBCPP_STD_VER >= 23

_LIBCPP_END_NAMESPACE_STD

_LIBCPP_POP_MACROS

#endif // _LIBCPP___MDSPAN_LAYOUTS_H

// =============================================================================
// FILE: include/__mdspan/default_accessor.h
// =============================================================================
// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
//===---------------------------------------------------------------------===//

#ifndef _LIBCPP___MDSPAN_DEFAULT_ACCESSOR_H
#define _LIBCPP___MDSPAN_DEFAULT_ACCESSOR_H

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#pragma GCC system_header
#endif

_LIBCPP_PUSH_MACROS

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER >= 23

template <class _ElementType> struct default_accessor {
    static_assert(!is_array_v<_ElementType>, "default_accessor: template argument may not be an array type");
    static_assert(!is_abstract_v<_ElementType>, "default_accessor: template argument may not be an abstract class");

    using offset_policy    = default_accessor;
    using element_type     = _ElementType;
    using reference        = _ElementType&;
    using data_handle_type = _ElementType*;

    _LIBCPP_HIDE_FROM_ABI constexpr default_accessor() noexcept = default;
    template <class _OtherElementType>
        requires(is_convertible_v<_OtherElementType (*)[], element_type (*)[]>)
    _LIBCPP_HIDE_FROM_ABI constexpr default_accessor(default_accessor<_OtherElementType>) noexcept {}

    _LIBCPP_HIDE_FROM_ABI constexpr reference access(data_handle_type __p, size_t __i) const noexcept {
        return __p[__i];
    }
    _LIBCPP_HIDE_FROM_ABI constexpr data_handle_type offset(data_handle_type __p, size_t __i) const noexcept {
        return __p + __i;
    }
};

#endif // _LIBCPP_STD_VER >= 23

_LIBCPP_END_NAMESPACE_STD

_LIBCPP_POP_MACROS

#endif // _LIBCPP___MDSPAN_DEFAULT_ACCESSOR_H

// =============================================================================
// FILE: include/__mdspan/aligned_accessor.h
// =============================================================================
// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
//===---------------------------------------------------------------------===//

#ifndef _LIBCPP___MDSPAN_ALIGNED_ACCESSOR_H
#define _LIBCPP___MDSPAN_ALIGNED_ACCESSOR_H

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#pragma GCC system_header
#endif

_LIBCPP_PUSH_MACROS

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER >= 26

template <class _ElementType, size_t _ByteAlignment> struct aligned_accessor {
    static_assert(_ByteAlignment != 0 && (_ByteAlignment & (_ByteAlignment - 1)) == 0,
        "aligned_accessor: byte alignment must be a power of two");
    static_assert(_ByteAlignment >= alignof(_ElementType), "aligned_accessor: insufficient byte alignment");
    static_assert(!is_array_v<_ElementType>, "aligned_accessor: template argument may not be an array type");
    static_assert(!is_abstract_v<_ElementType>, "aligned_accessor: template argument may not be an abstract class");

    using offset_policy    = default_accessor<_ElementType>;
    using element_type     = _ElementType;
    using reference        = _ElementType&;
    using data_handle_type = _ElementType*;

    static constexpr size_t byte_alignment = _ByteAlignment;

    _LIBCPP_HIDE_FROM_ABI constexpr aligned_accessor() noexcept = default;

    template <class _OtherElementType, size_t _OtherByteAlignment>
        requires(is_convertible_v<_OtherElementType (*)[], element_type (*)[]> && _OtherByteAlignment >= byte_alignment)
    _LIBCPP_HIDE_FROM_ABI constexpr aligned_accessor(
        aligned_accessor<_OtherElementType, _OtherByteAlignment>) noexcept {}

    template <class _OtherElementType>
        requires(is_convertible_v<_OtherElementType (*)[], element_type (*)[]>)
    _LIBCPP_HIDE_FROM_ABI explicit constexpr aligned_accessor(default_accessor<_OtherElementType>) noexcept {}

    template <class _OtherElementType>
        requires(is_convertible_v<element_type (*)[], _OtherElementType (*)[]>)
    _LIBCPP_HIDE_FROM_ABI constexpr operator default_accessor<_OtherElementType>() const noexcept {
        return {};
    }

    _LIBCPP_HIDE_FROM_ABI constexpr reference access(data_handle_type __p, size_t __i) const noexcept {
        return std::assume_aligned<byte_alignment>(__p)[__i];
    }

    _LIBCPP_HIDE_FROM_ABI constexpr typename offset_policy::data_handle_type offset(
        data_handle_type __p, size_t __i) const noexcept {
        return std::assume_aligned<byte_alignment>(__p) + __i;
    }
};

#endif // _LIBCPP_STD_VER >= 26

_LIBCPP_END_NAMESPACE_STD

_LIBCPP_POP_MACROS

#endif // _LIBCPP___MDSPAN_ALIGNED_ACCESSOR_H

// =============================================================================
// FILE: include/__mdspan/extents.h
// =============================================================================
// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
//===---------------------------------------------------------------------===//

#ifndef _LIBCPP___MDSPAN_EXTENTS_H
#define _LIBCPP___MDSPAN_EXTENTS_H

#include <array>
#include <concepts>
#include <limits>
#include <span>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#pragma GCC system_header
#endif

_LIBCPP_PUSH_MACROS

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER >= 23

namespace __mdspan_detail {
// ------------------------------------------------------------------
// ------------ __possibly_empty_array  -----------------------------
// ------------------------------------------------------------------

// array like class which provides get function and operator [], and
// has a specialization for the size 0 case.
// This is needed to make the __maybe_static_array be truly empty, for
// all static values.

template <class _Tp, size_t _Size> struct __possibly_empty_array {
    _Tp __vals_[_Size];
    _LIBCPP_HIDE_FROM_ABI constexpr _Tp& operator[](size_t __index) { return __vals_[__index]; }
    _LIBCPP_HIDE_FROM_ABI constexpr const _Tp& operator[](size_t __index) const { return __vals_[__index]; }
};

template <class _Tp> struct __possibly_empty_array<_Tp, 0> {
    _LIBCPP_HIDE_FROM_ABI constexpr _Tp& operator[](size_t) { __libcpp_unreachable(); }
    _LIBCPP_HIDE_FROM_ABI constexpr const _Tp& operator[](size_t) const { __libcpp_unreachable(); }
};

// ------------------------------------------------------------------
// ------------ static_partial_sums ---------------------------------
// ------------------------------------------------------------------

// Provides a compile time partial sum one can index into

template <size_t... _Values> struct __static_partial_sums {
    _LIBCPP_HIDE_FROM_ABI static constexpr array<size_t, sizeof...(_Values)> __static_partial_sums_impl() {
        array<size_t, sizeof...(_Values)> __values { _Values... };
        array<size_t, sizeof...(_Values)> __partial_sums { {} };
        size_t __running_sum = 0;
        for (int __i = 0; __i != sizeof...(_Values); ++__i) {
            __partial_sums[__i] = __running_sum;
            __running_sum += __values[__i];
        }
        return __partial_sums;
    }
    static constexpr array<size_t, sizeof...(_Values)> __result { __static_partial_sums_impl() };

    _LIBCPP_HIDE_FROM_ABI static constexpr size_t __get(size_t __index) { return __result[__index]; }
};

// ------------------------------------------------------------------
// ------------ __maybe_static_array --------------------------------
// ------------------------------------------------------------------

// array like class which has a mix of static and runtime values but
// only stores the runtime values.
// The type of the static and the runtime values can be different.
// The position of a dynamic value is indicated by dynamic_extent.
template <class _TDynamic, class _TStatic, _TStatic... _Values> struct __maybe_static_array {
    static_assert(
        is_convertible_v<_TStatic, _TDynamic>, "__maybe_static_array: _TStatic must be convertible to _TDynamic");
    static_assert(
        is_convertible_v<_TDynamic, _TStatic>, "__maybe_static_array: _TDynamic must be convertible to _TStatic");

private:
    // Static values member
    static constexpr size_t __size_         = sizeof...(_Values);
    static constexpr size_t __size_dynamic_ = ((_Values == dynamic_extent) + ... + 0);
    using _DynamicValues _LIBCPP_NODEBUG    = __possibly_empty_array<_TDynamic, __size_dynamic_>;

    static constexpr array<_TStatic, sizeof...(_Values)> __static_values_ = { _Values... };
    _LIBCPP_NO_UNIQUE_ADDRESS _DynamicValues __dyn_vals_;

    // static mapping of indices to the position in the dynamic values array
    using _DynamicIdxMap _LIBCPP_NODEBUG = __static_partial_sums<static_cast<size_t>(_Values == dynamic_extent)...>;

public:
    _LIBCPP_HIDE_FROM_ABI constexpr __maybe_static_array() noexcept
        : __dyn_vals_ {} {}

    // constructors from dynamic values only -- this covers the case for rank() == 0
    template <class... _DynVals>
        requires(sizeof...(_DynVals) == __size_dynamic_)
    _LIBCPP_HIDE_FROM_ABI constexpr __maybe_static_array(_DynVals... __vals)
        : __dyn_vals_ { static_cast<_TDynamic>(__vals)... } {}

    template <class _Tp, size_t _Size>
        requires(_Size == __size_dynamic_)
    _LIBCPP_HIDE_FROM_ABI constexpr __maybe_static_array([[maybe_unused]] const span<_Tp, _Size>& __vals) {
        if constexpr (_Size > 0) {
            for (size_t __i = 0; __i < _Size; __i++) {
                __dyn_vals_[__i] = static_cast<_TDynamic>(__vals[__i]);
            }
        }
    }

    // constructors from all values -- here rank will be greater than 0
    template <class... _DynVals>
        requires(sizeof...(_DynVals) != __size_dynamic_)
    _LIBCPP_HIDE_FROM_ABI constexpr __maybe_static_array(_DynVals... __vals) {
        static_assert(sizeof...(_DynVals) == __size_, "Invalid number of values.");
        _TDynamic __values[__size_] = { static_cast<_TDynamic>(__vals)... };
        for (size_t __i = 0; __i < __size_; __i++) {
            _TStatic __static_val = __static_values_[__i];
            if (__static_val == dynamic_extent) {
                __dyn_vals_[_DynamicIdxMap::__get(__i)] = __values[__i];
            } else {
                // Not catching this could lead to out of bounds errors later
                // e.g. using my_mdspan_t = mdspan<int, extents<int, 10>>; my_mdspan_t = m(new int[5], 5);
                // Right-hand-side construction looks ok with allocation and size matching,
                // but since (potentially elsewhere defined) my_mdspan_t has static size m now thinks its range is 10
                // not 5
                _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__values[__i] == static_cast<_TDynamic>(__static_val),
                    "extents construction: mismatch of provided arguments with static extents.");
            }
        }
    }

    template <class _Tp, size_t _Size>
        requires(_Size != __size_dynamic_)
    _LIBCPP_HIDE_FROM_ABI constexpr __maybe_static_array(const span<_Tp, _Size>& __vals) {
        static_assert(_Size == __size_ || __size_ == dynamic_extent);
        for (size_t __i = 0; __i < __size_; __i++) {
            _TStatic __static_val = __static_values_[__i];
            if (__static_val == dynamic_extent) {
                __dyn_vals_[_DynamicIdxMap::__get(__i)] = static_cast<_TDynamic>(__vals[__i]);
            } else {
                // Not catching this could lead to out of bounds errors later
                // e.g. using my_mdspan_t = mdspan<int, extents<int, 10>>; my_mdspan_t = m(new int[N], span<int,1>(&N));
                // Right-hand-side construction looks ok with allocation and size matching,
                // but since (potentially elsewhere defined) my_mdspan_t has static size m now thinks its range is 10
                // not N
                _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
                    static_cast<_TDynamic>(__vals[__i]) == static_cast<_TDynamic>(__static_val),
                    "extents construction: mismatch of provided arguments with static extents.");
            }
        }
    }

    // access functions
    _LIBCPP_HIDE_FROM_ABI static constexpr _TStatic __static_value(size_t __i) noexcept {
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__i < __size_, "extents access: index must be less than rank");
        return __static_values_[__i];
    }

    _LIBCPP_HIDE_FROM_ABI constexpr _TDynamic __value(size_t __i) const {
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__i < __size_, "extents access: index must be less than rank");
        _TStatic __static_val = __static_values_[__i];
        return __static_val == dynamic_extent ? __dyn_vals_[_DynamicIdxMap::__get(__i)]
                                              : static_cast<_TDynamic>(__static_val);
    }
    _LIBCPP_HIDE_FROM_ABI constexpr _TDynamic operator[](size_t __i) const {
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__i < __size_, "extents access: index must be less than rank");
        return __value(__i);
    }

    // observers
    _LIBCPP_HIDE_FROM_ABI static constexpr size_t __size() { return __size_; }
    _LIBCPP_HIDE_FROM_ABI static constexpr size_t __size_dynamic() { return __size_dynamic_; }
};

// Function to check whether a value is representable as another type
// value must be a positive integer otherwise returns false
// if _From is not an integral, we just check positivity
template <integral _To, class _From>
    requires(integral<_From>)
_LIBCPP_HIDE_FROM_ABI constexpr bool __is_representable_as(_From __value) {
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
_LIBCPP_HIDE_FROM_ABI constexpr bool __is_representable_as(_From __value) {
    if constexpr (is_signed_v<_To>) {
        if (static_cast<_To>(__value) < 0) { return false; }
    }
    return true;
}

} // namespace __mdspan_detail

// ------------------------------------------------------------------
// ------------ extents ---------------------------------------------
// ------------------------------------------------------------------

// Class to describe the extents of a multi dimensional array.
// Used by mdspan, mdarray and layout mappings.
// See ISO C++ standard [mdspan.extents]

template <class _IndexType, size_t... _Extents> class extents {
public:
    // typedefs for integral types used
    using index_type = _IndexType;
    using size_type  = make_unsigned_t<index_type>;
    using rank_type  = size_t;

    static_assert(
        __signed_or_unsigned_integer<index_type>, "extents::index_type must be a signed or unsigned integer type");
    static_assert(
        ((__mdspan_detail::__is_representable_as<index_type>(_Extents) || (_Extents == dynamic_extent)) && ...),
        "extents ctor: arguments must be representable as index_type and nonnegative");

private:
    static constexpr rank_type __rank_         = sizeof...(_Extents);
    static constexpr rank_type __rank_dynamic_ = ((_Extents == dynamic_extent) + ... + 0);

    // internal storage type using __maybe_static_array
    using _Values _LIBCPP_NODEBUG = __mdspan_detail::__maybe_static_array<_IndexType, size_t, _Extents...>;
    [[no_unique_address]] _Values __vals_;

    template <class _OtherIndexType>
    _LIBCPP_HIDE_FROM_ABI static constexpr index_type __checked_index_cast(_OtherIndexType&& __value) noexcept {
        using _OtherType = remove_cvref_t<_OtherIndexType>;
        if constexpr (integral<_OtherType> && !same_as<_OtherType, bool>) {
            _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__mdspan_detail::__is_representable_as<index_type>(__value),
                "extents ctor: arguments must be representable as index_type and nonnegative");
            return static_cast<index_type>(__value);
        } else {
            auto __converted_val = static_cast<index_type>(std::forward<_OtherIndexType>(__value));
            if constexpr (is_signed_v<index_type>) {
                _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__converted_val >= 0,
                    "extents ctor: arguments must be representable as index_type and nonnegative");
            }
            return __converted_val;
        }
    }

    template <class... _OtherIndexTypes>
    _LIBCPP_HIDE_FROM_ABI static constexpr _Values __representability_checked_cast(
        _OtherIndexTypes&&... __values) noexcept {
        return _Values { __checked_index_cast(std::forward<_OtherIndexTypes>(__values))... };
    }

    template <class _OtherIndexType, size_t _Size, size_t... _Idxs>
    _LIBCPP_HIDE_FROM_ABI static constexpr _Values __representability_checked_cast(
        const array<_OtherIndexType, _Size>& __exts, index_sequence<_Idxs...>) noexcept {
        return __representability_checked_cast(__exts[_Idxs]...);
    }

    template <class _OtherIndexType, size_t _Size, size_t... _Idxs>
    _LIBCPP_HIDE_FROM_ABI static constexpr _Values __representability_checked_cast(
        const span<_OtherIndexType, _Size>& __exts, index_sequence<_Idxs...>) noexcept {
        return __representability_checked_cast(std::as_const(__exts[_Idxs])...);
    }

public:
    // [mdspan.extents.obs], observers of multidimensional index space
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr rank_type rank() noexcept { return __rank_; }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr rank_type rank_dynamic() noexcept { return __rank_dynamic_; }

    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr index_type extent(rank_type __r) const noexcept {
        return __vals_.__value(__r);
    }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr size_t static_extent(rank_type __r) noexcept {
        return _Values::__static_value(__r);
    }

    // [mdspan.extents.cons], constructors
    _LIBCPP_HIDE_FROM_ABI constexpr extents() noexcept = default;

    // Construction from just dynamic or all values.
    // Precondition check is deferred to __maybe_static_array constructor
    template <class... _OtherIndexTypes>
        requires((is_convertible_v<_OtherIndexTypes, index_type> && ...)
            && (is_nothrow_constructible_v<index_type, _OtherIndexTypes> && ...)
            && (sizeof...(_OtherIndexTypes) == __rank_ || sizeof...(_OtherIndexTypes) == __rank_dynamic_))
    _LIBCPP_HIDE_FROM_ABI constexpr explicit extents(_OtherIndexTypes... __dynvals) noexcept
        // Not catching this could lead to out of bounds errors later
        // e.g. mdspan m(ptr, dextents<char, 1>(200u)); leads to an extent of -56 on m
        : __vals_(__representability_checked_cast(std::move(__dynvals)...)) {}

    template <class _OtherIndexType, size_t _Size>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
            && is_nothrow_constructible_v<index_type, const _OtherIndexType&>
            && (_Size == __rank_ || _Size == __rank_dynamic_))
    explicit(_Size != __rank_dynamic_) _LIBCPP_HIDE_FROM_ABI
        constexpr extents(const array<_OtherIndexType, _Size>& __exts) noexcept
        // Not catching this could lead to out of bounds errors later
        // e.g. mdspan m(ptr, dextents<char, 1>(200u)); leads to an extent of -56 on m
        : __vals_(__representability_checked_cast(__exts, make_index_sequence<_Size>())) {}

    template <class _OtherIndexType, size_t _Size>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
            && is_nothrow_constructible_v<index_type, const _OtherIndexType&>
            && (_Size == __rank_ || _Size == __rank_dynamic_))
    explicit(_Size != __rank_dynamic_) _LIBCPP_HIDE_FROM_ABI
        constexpr extents(const span<_OtherIndexType, _Size>& __exts) noexcept
        // Not catching this could lead to out of bounds errors later
        // e.g. mdspan m(ptr, dextents<char, 1>(200u)); leads to an extent of -56 on m
        : __vals_(__representability_checked_cast(__exts, make_index_sequence<_Size>())) {}

private:
    // Function to construct extents storage from other extents.
    template <size_t _DynCount, size_t _Idx, class _OtherExtents, class... _DynamicValues>
        requires(_Idx < __rank_)
    _LIBCPP_HIDE_FROM_ABI constexpr _Values __construct_vals_from_extents(integral_constant<size_t, _DynCount>,
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
    _LIBCPP_HIDE_FROM_ABI constexpr _Values __construct_vals_from_extents(integral_constant<size_t, _DynCount>,
        integral_constant<size_t, _Idx>, const _OtherExtents&, _DynamicValues... __dynamic_values) noexcept {
        return _Values { static_cast<index_type>(__dynamic_values)... };
    }

public:
    // Converting constructor from other extents specializations
    template <class _OtherIndexType, size_t... _OtherExtents>
        requires((sizeof...(_OtherExtents) == sizeof...(_Extents))
            && ((_OtherExtents == dynamic_extent || _Extents == dynamic_extent || _OtherExtents == _Extents) && ...))
    explicit((((_Extents != dynamic_extent) && (_OtherExtents == dynamic_extent)) || ...)
        || (static_cast<make_unsigned_t<index_type>>(numeric_limits<index_type>::max())
            < static_cast<make_unsigned_t<_OtherIndexType>>(
                numeric_limits<_OtherIndexType>::max()))) _LIBCPP_HIDE_FROM_ABI
        constexpr extents(const extents<_OtherIndexType, _OtherExtents...>& __other) noexcept
        : __vals_(
              __construct_vals_from_extents(integral_constant<size_t, 0>(), integral_constant<size_t, 0>(), __other)) {
        if constexpr (rank() > 0) {
            for (size_t __r = 0; __r < rank(); __r++) {
                if constexpr (static_cast<make_unsigned_t<index_type>>(numeric_limits<index_type>::max())
                    < static_cast<make_unsigned_t<_OtherIndexType>>(numeric_limits<_OtherIndexType>::max())) {
                    // Not catching this could lead to out of bounds errors later
                    // e.g. dextents<char,1>> e(dextents<unsigned,1>(200)) leads to an extent of -56 on e
                    _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
                        __mdspan_detail::__is_representable_as<index_type>(__other.extent(__r)),
                        "extents ctor: arguments must be representable as index_type and nonnegative");
                }
                // Not catching this could lead to out of bounds errors later
                // e.g. mdspan<int, extents<int, 10>> m = mdspan<int, dextents<int, 1>>(new int[5], 5);
                // Right-hand-side construction was ok, but m now thinks its range is 10 not 5
                _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS((_Values::__static_value(__r) == dynamic_extent)
                        || (static_cast<index_type>(__other.extent(__r))
                            == static_cast<index_type>(_Values::__static_value(__r))),
                    "extents construction: mismatch of provided arguments with static extents.");
            }
        }
    }

    // Comparison operator
    template <class _OtherIndexType, size_t... _OtherExtents>
    _LIBCPP_HIDE_FROM_ABI friend constexpr bool operator==(
        const extents& __lhs, const extents<_OtherIndexType, _OtherExtents...>& __rhs) noexcept {
        if constexpr (rank() != sizeof...(_OtherExtents)) {
            return false;
        } else {
            for (rank_type __r = 0; __r < __rank_; __r++) {
                // avoid warning when comparing signed and unsigner integers and pick the wider of two types
                using _CommonType = common_type_t<index_type, _OtherIndexType>;
                if (static_cast<_CommonType>(__lhs.extent(__r)) != static_cast<_CommonType>(__rhs.extent(__r))) {
                    return false;
                }
            }
        }
        return true;
    }

    template <class _OtherIndexType>
    _LIBCPP_HIDE_FROM_ABI static constexpr auto __index_cast(_OtherIndexType&& __i) noexcept {
        using _OtherIndex = remove_cvref_t<_OtherIndexType>;
        if constexpr (integral<_OtherIndex> && !same_as<_OtherIndex, bool>) {
            return __i;
        } else {
            return static_cast<index_type>(std::forward<_OtherIndexType>(__i));
        }
    }
};

// Recursive helper classes to implement dextents alias for extents
namespace __mdspan_detail {

template <class _IndexType, size_t _Rank, class _Extents = extents<_IndexType>> struct __make_dextents;

template <class _IndexType, size_t _Rank, size_t... _ExtentsPack>
struct __make_dextents<_IndexType, _Rank, extents<_IndexType, _ExtentsPack...>> {
    using type _LIBCPP_NODEBUG =
        typename __make_dextents<_IndexType, _Rank - 1, extents<_IndexType, dynamic_extent, _ExtentsPack...>>::type;
};

template <class _IndexType, size_t... _ExtentsPack>
struct __make_dextents<_IndexType, 0, extents<_IndexType, _ExtentsPack...>> {
    using type _LIBCPP_NODEBUG = extents<_IndexType, _ExtentsPack...>;
};

} // namespace __mdspan_detail

// [mdspan.extents.dextents], alias template
template <class _IndexType, size_t _Rank>
using dextents = typename __mdspan_detail::__make_dextents<_IndexType, _Rank>::type;

#if _LIBCPP_STD_VER >= 26
// [mdspan.extents.dims], alias template `dims`
template <size_t _Rank, class _IndexType = size_t> using dims = dextents<_IndexType, _Rank>;
#endif

// Deduction guide for extents
#if _LIBCPP_STD_VER >= 26
template <class... _IndexTypes>
    requires(is_convertible_v<_IndexTypes, size_t> && ...)
explicit extents(_IndexTypes...) -> extents<size_t, __maybe_static_ext<_IndexTypes>...>;
#else
template <class... _IndexTypes>
    requires(is_convertible_v<_IndexTypes, size_t> && ...)
explicit extents(_IndexTypes...) -> extents<size_t, size_t(((void)sizeof(_IndexTypes), dynamic_extent))...>;
#endif

namespace __mdspan_detail {

// Helper type traits for identifying a class as extents.
template <class _Tp> struct __is_extents : false_type {};

template <class _IndexType, size_t... _ExtentsPack>
struct __is_extents<extents<_IndexType, _ExtentsPack...>> : true_type {};

template <class _Tp> inline constexpr bool __is_extents_v = __is_extents<_Tp>::value;

// Function to check whether a set of indices are a multidimensional
// index into extents. This is a word of power in the C++ standard
// requiring that the indices are larger than 0 and smaller than
// the respective extents.

template <integral _IndexType, class _From>
    requires(integral<_From>)
_LIBCPP_HIDE_FROM_ABI constexpr bool __is_index_in_extent(_IndexType __extent, _From __value) {
    if constexpr (is_signed_v<_From>) {
        if (__value < 0) { return false; }
    }
    using _Tp = common_type_t<_IndexType, _From>;
    return static_cast<_Tp>(__value) < static_cast<_Tp>(__extent);
}

template <integral _IndexType, class _From>
    requires(!integral<_From>)
_LIBCPP_HIDE_FROM_ABI constexpr bool __is_index_in_extent(_IndexType __extent, _From __value) {
    if constexpr (is_signed_v<_IndexType>) {
        if (static_cast<_IndexType>(__value) < 0) { return false; }
    }
    return static_cast<_IndexType>(__value) < __extent;
}

template <size_t... _Idxs, class _Extents, class... _From>
_LIBCPP_HIDE_FROM_ABI constexpr bool __is_multidimensional_index_in_impl(
    index_sequence<_Idxs...>, const _Extents& __ext, _From... __values) {
    return (__mdspan_detail::__is_index_in_extent(__ext.extent(_Idxs), __values) && ...);
}

template <class _Extents, class... _From>
_LIBCPP_HIDE_FROM_ABI constexpr bool __is_multidimensional_index_in(const _Extents& __ext, _From... __values) {
    return __mdspan_detail::__is_multidimensional_index_in_impl(
        make_index_sequence<_Extents::rank()>(), __ext, __values...);
}

} // namespace __mdspan_detail

#endif // _LIBCPP_STD_VER >= 23

_LIBCPP_END_NAMESPACE_STD

_LIBCPP_POP_MACROS

#endif // _LIBCPP___MDSPAN_EXTENTS_H

// =============================================================================
// FILE: include/__mdspan/layout_left.h
// =============================================================================
// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
//===---------------------------------------------------------------------===//

#ifndef _LIBCPP___MDSPAN_LAYOUT_LEFT_H
#define _LIBCPP___MDSPAN_LAYOUT_LEFT_H

#include <array>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#pragma GCC system_header
#endif

_LIBCPP_PUSH_MACROS

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER >= 23

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
    _LIBCPP_HIDE_FROM_ABI static constexpr bool __required_span_size_is_representable(const extents_type& __ext) {
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
    // [mdspan.layout.left.cons], constructors
    _LIBCPP_HIDE_FROM_ABI constexpr mapping() noexcept               = default;
    _LIBCPP_HIDE_FROM_ABI constexpr mapping(const mapping&) noexcept = default;
    _LIBCPP_HIDE_FROM_ABI constexpr mapping(const extents_type& __ext) noexcept
        : __extents_(__ext) {
        // not catching this could lead to out-of-bounds access later when used inside mdspan
        // mapping<dextents<char, 2>> map(dextents<char, 2>(40,40)); map(10, 3) == -126
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__required_span_size_is_representable(__ext),
            "layout_left::mapping extents ctor: product of extents must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents>)
    _LIBCPP_HIDE_FROM_ABI constexpr explicit(!is_convertible_v<_OtherExtents, extents_type>)
        mapping(const mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        // not catching this could lead to out-of-bounds access later when used inside mdspan
        // mapping<dextents<char, 2>> map(mapping<dextents<int, 2>>(dextents<int, 2>(40,40))); map(10, 3) == -126
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_left::mapping converting ctor: other.required_span_size() must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents> && _OtherExtents::rank() <= 1)
    _LIBCPP_HIDE_FROM_ABI constexpr explicit(!is_convertible_v<_OtherExtents, extents_type>)
        mapping(const layout_right::mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        // not catching this could lead to out-of-bounds access later when used inside mdspan
        // Note: since this is constraint to rank 1, extents itself would catch the invalid conversion first
        //       and thus this assertion should never be triggered, but keeping it here for consistency
        // layout_left::mapping<dextents<char, 1>> map(
        //           layout_right::mapping<dextents<unsigned, 1>>(dextents<unsigned, 1>(200))); map.extents().extent(0)
        //           == -56
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_left::mapping converting ctor: other.required_span_size() must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents>)
    _LIBCPP_HIDE_FROM_ABI constexpr explicit(extents_type::rank() > 0)
        mapping(const layout_stride::mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        if constexpr (extents_type::rank() > 0) {
            _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(([&]() {
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
            _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
                __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
                "layout_left::mapping from layout_stride ctor: other.required_span_size() must be representable as "
                "index_type.");
        }
    }

    _LIBCPP_HIDE_FROM_ABI constexpr mapping& operator=(const mapping&) noexcept = default;

    // [mdspan.layout.left.obs], observers
    _LIBCPP_HIDE_FROM_ABI constexpr const extents_type& extents() const noexcept { return __extents_; }

    _LIBCPP_HIDE_FROM_ABI constexpr index_type required_span_size() const noexcept {
        index_type __size = 1;
        for (size_t __r = 0; __r < extents_type::rank(); __r++) {
            __size *= __extents_.extent(__r);
        }
        return __size;
    }

    template <class... _Indices>
        requires((sizeof...(_Indices) == extents_type::rank()) && (is_convertible_v<_Indices, index_type> && ...)
            && (is_nothrow_constructible_v<index_type, _Indices> && ...))
    _LIBCPP_HIDE_FROM_ABI constexpr index_type operator()(_Indices... __idx) const noexcept {
        // Mappings are generally meant to be used for accessing allocations and are meant to guarantee to never
        // return a value exceeding required_span_size(), which is used to know how large an allocation one needs
        // Thus, this is a canonical point in multi-dimensional data structures to make invalid element access checks
        // However, mdspan does check this on its own, so for now we avoid double checking in hardened mode
        _LIBCPP_ASSERT_UNCATEGORIZED(__mdspan_detail::__is_multidimensional_index_in(__extents_, __idx...),
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

    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_unique() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_exhaustive() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_strided() noexcept { return true; }

    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_unique() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_exhaustive() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_strided() noexcept { return true; }

    _LIBCPP_HIDE_FROM_ABI constexpr index_type stride(rank_type __r) const noexcept
        requires(extents_type::rank() > 0)
    {
        // While it would be caught by extents itself too, using a too large __r
        // is functionally an out of bounds access on the stored information needed to compute strides
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
            __r < extents_type::rank(), "layout_left::mapping::stride(): invalid rank index");
        index_type __s = 1;
        for (rank_type __i = 0; __i < __r; __i++) {
            __s *= __extents_.extent(__i);
        }
        return __s;
    }

    template <class _OtherExtents>
        requires(_OtherExtents::rank() == extents_type::rank())
    _LIBCPP_HIDE_FROM_ABI friend constexpr bool operator==(
        const mapping& __lhs, const mapping<_OtherExtents>& __rhs) noexcept {
        return __lhs.extents() == __rhs.extents();
    }

private:
    _LIBCPP_NO_UNIQUE_ADDRESS extents_type __extents_ {};
};

#endif // _LIBCPP_STD_VER >= 23

_LIBCPP_END_NAMESPACE_STD

_LIBCPP_POP_MACROS

#endif // _LIBCPP___MDSPAN_LAYOUT_LEFT_H

// =============================================================================
// FILE: include/__mdspan/layout_right.h
// =============================================================================
// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
//===---------------------------------------------------------------------===//

#ifndef _LIBCPP___MDSPAN_LAYOUT_RIGHT_H
#define _LIBCPP___MDSPAN_LAYOUT_RIGHT_H

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#pragma GCC system_header
#endif

_LIBCPP_PUSH_MACROS

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER >= 23

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
    _LIBCPP_HIDE_FROM_ABI static constexpr bool __required_span_size_is_representable(const extents_type& __ext) {
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
    // [mdspan.layout.right.cons], constructors
    _LIBCPP_HIDE_FROM_ABI constexpr mapping() noexcept               = default;
    _LIBCPP_HIDE_FROM_ABI constexpr mapping(const mapping&) noexcept = default;
    _LIBCPP_HIDE_FROM_ABI constexpr mapping(const extents_type& __ext) noexcept
        : __extents_(__ext) {
        // not catching this could lead to out-of-bounds access later when used inside mdspan
        // mapping<dextents<char, 2>> map(dextents<char, 2>(40,40)); map(3, 10) == -126
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__required_span_size_is_representable(__ext),
            "layout_right::mapping extents ctor: product of extents must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents>)
    _LIBCPP_HIDE_FROM_ABI constexpr explicit(!is_convertible_v<_OtherExtents, extents_type>)
        mapping(const mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        // not catching this could lead to out-of-bounds access later when used inside mdspan
        // mapping<dextents<char, 2>> map(mapping<dextents<int, 2>>(dextents<int, 2>(40,40))); map(3, 10) == -126
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_right::mapping converting ctor: other.required_span_size() must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents> && _OtherExtents::rank() <= 1)
    _LIBCPP_HIDE_FROM_ABI constexpr explicit(!is_convertible_v<_OtherExtents, extents_type>)
        mapping(const layout_left::mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        // not catching this could lead to out-of-bounds access later when used inside mdspan
        // Note: since this is constraint to rank 1, extents itself would catch the invalid conversion first
        //       and thus this assertion should never be triggered, but keeping it here for consistency
        // layout_right::mapping<dextents<char, 1>> map(
        //           layout_left::mapping<dextents<unsigned, 1>>(dextents<unsigned, 1>(200))); map.extents().extent(0)
        //           == -56
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_right::mapping converting ctor: other.required_span_size() must be representable as index_type.");
    }

    template <class _OtherExtents>
        requires(is_constructible_v<extents_type, _OtherExtents>)
    _LIBCPP_HIDE_FROM_ABI constexpr explicit(extents_type::rank() > 0)
        mapping(const layout_stride::mapping<_OtherExtents>& __other) noexcept
        : __extents_(__other.extents()) {
        if constexpr (extents_type::rank() > 0) {
            _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(([&]() {
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
            _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
                __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
                "layout_right::mapping from layout_stride ctor: other.required_span_size() must be representable as "
                "index_type.");
        }
    }

    _LIBCPP_HIDE_FROM_ABI constexpr mapping& operator=(const mapping&) noexcept = default;

    // [mdspan.layout.right.obs], observers
    _LIBCPP_HIDE_FROM_ABI constexpr const extents_type& extents() const noexcept { return __extents_; }

    _LIBCPP_HIDE_FROM_ABI constexpr index_type required_span_size() const noexcept {
        index_type __size = 1;
        for (size_t __r = 0; __r < extents_type::rank(); __r++) {
            __size *= __extents_.extent(__r);
        }
        return __size;
    }

    template <class... _Indices>
        requires((sizeof...(_Indices) == extents_type::rank()) && (is_convertible_v<_Indices, index_type> && ...)
            && (is_nothrow_constructible_v<index_type, _Indices> && ...))
    _LIBCPP_HIDE_FROM_ABI constexpr index_type operator()(_Indices... __idx) const noexcept {
        // Mappings are generally meant to be used for accessing allocations and are meant to guarantee to never
        // return a value exceeding required_span_size(), which is used to know how large an allocation one needs
        // Thus, this is a canonical point in multi-dimensional data structures to make invalid element access checks
        // However, mdspan does check this on its own, so for now we avoid double checking in hardened mode
        _LIBCPP_ASSERT_UNCATEGORIZED(__mdspan_detail::__is_multidimensional_index_in(__extents_, __idx...),
            "layout_right::mapping: out of bounds indexing");
        return [&]<size_t... _Pos>(index_sequence<_Pos...>) {
            index_type __res = 0;
            ((__res = static_cast<index_type>(__idx) + __extents_.extent(_Pos) * __res), ...);
            return __res;
        }(make_index_sequence<sizeof...(_Indices)>());
    }

    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_unique() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_exhaustive() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_strided() noexcept { return true; }

    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_unique() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_exhaustive() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_strided() noexcept { return true; }

    _LIBCPP_HIDE_FROM_ABI constexpr index_type stride(rank_type __r) const noexcept
        requires(extents_type::rank() > 0)
    {
        // While it would be caught by extents itself too, using a too large __r
        // is functionally an out of bounds access on the stored information needed to compute strides
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
            __r < extents_type::rank(), "layout_right::mapping::stride(): invalid rank index");
        index_type __s = 1;
        for (rank_type __i = extents_type::rank() - 1; __i > __r; __i--) {
            __s *= __extents_.extent(__i);
        }
        return __s;
    }

    template <class _OtherExtents>
        requires(_OtherExtents::rank() == extents_type::rank())
    _LIBCPP_HIDE_FROM_ABI friend constexpr bool operator==(
        const mapping& __lhs, const mapping<_OtherExtents>& __rhs) noexcept {
        return __lhs.extents() == __rhs.extents();
    }

private:
    _LIBCPP_NO_UNIQUE_ADDRESS extents_type __extents_ {};
};

#endif // _LIBCPP_STD_VER >= 23

_LIBCPP_END_NAMESPACE_STD

_LIBCPP_POP_MACROS

#endif // _LIBCPP___MDSPAN_LAYOUT_RIGHT_H

// =============================================================================
// FILE: include/__mdspan/layout_stride.h
// =============================================================================
// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
//===---------------------------------------------------------------------===//

#ifndef _LIBCPP___MDSPAN_LAYOUT_STRIDE_H
#define _LIBCPP___MDSPAN_LAYOUT_STRIDE_H

#include <array>
#include <limits>
#include <span>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#pragma GCC system_header
#endif

_LIBCPP_PUSH_MACROS

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER >= 23

namespace __mdspan_detail {
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

    // Used for default construction check and mandates
    _LIBCPP_HIDE_FROM_ABI static constexpr bool __required_span_size_is_representable(const extents_type& __ext) {
        if constexpr (__rank_ == 0) { return true; }

        index_type __prod = __ext.extent(0);
        for (rank_type __r = 1; __r < __rank_; __r++) {
            bool __overflowed = __builtin_mul_overflow(__prod, __ext.extent(__r), std::addressof(__prod));
            if (__overflowed) { return false; }
        }
        return true;
    }

    template <class _OtherIndexType>
    _LIBCPP_HIDE_FROM_ABI static constexpr bool __required_span_size_is_representable(
        const extents_type& __ext, span<_OtherIndexType, __rank_> __strides) {
        if constexpr (__rank_ == 0) { return true; }

        index_type __size = 1;
        for (rank_type __r = 0; __r < __rank_; __r++) {
            // We can only check correct conversion of _OtherIndexType if it is an integral
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

    // compute offset of a strided layout mapping
    template <class _StridedMapping>
    _LIBCPP_HIDE_FROM_ABI static constexpr index_type __offset(const _StridedMapping& __mapping) {
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

    // compute the permutation for sorting the stride array
    // we never actually sort the stride array
    _LIBCPP_HIDE_FROM_ABI constexpr void __bubble_sort_by_strides(array<rank_type, __rank_>& __permute) const {
        for (rank_type __i = __rank_ - 1; __i > 0; __i--) {
            for (rank_type __r = 0; __r < __i; __r++) {
                if (__strides_[__permute[__r]] > __strides_[__permute[__r + 1]]) {
                    swap(__permute[__r], __permute[__r + 1]);
                } else {
                    // if two strides are the same then one of the associated extents must be 1 or 0
                    // both could be, but you can't have one larger than 1 come first
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
    // [mdspan.layout.stride.cons], constructors
    _LIBCPP_HIDE_FROM_ABI constexpr mapping() noexcept
        : __extents_(extents_type()) {
        // Note the nominal precondition is covered by above static assert since
        // if rank_dynamic is != 0 required_span_size is zero for default construction
        if constexpr (__rank_ > 0) {
            index_type __stride = 1;
            for (rank_type __r = __rank_ - 1; __r > static_cast<rank_type>(0); __r--) {
                __strides_[__r] = __stride;
                __stride *= __extents_.extent(__r);
            }
            __strides_[0] = __stride;
        }
    }

    _LIBCPP_HIDE_FROM_ABI constexpr mapping(const mapping&) noexcept = default;

    template <class _OtherIndexType>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
                    && is_nothrow_constructible_v<index_type, const _OtherIndexType&>)
    _LIBCPP_HIDE_FROM_ABI constexpr mapping(
        const extents_type& __ext, span<_OtherIndexType, __rank_> __strides) noexcept
        : __extents_(__ext)
        , __strides_([&]<size_t... _Pos>(index_sequence<_Pos...>) {
            return __mdspan_detail::__possibly_empty_array<index_type, __rank_> { static_cast<index_type>(
                std::as_const(__strides[_Pos]))... };
        }(make_index_sequence<__rank_>())) {
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(([&]<size_t... _Pos>(index_sequence<_Pos...>) {
            // For integrals we can do a pre-conversion check, for other types not
            if constexpr (is_integral_v<_OtherIndexType>) {
                return ((__strides[_Pos] > static_cast<_OtherIndexType>(0)) && ... && true);
            } else {
                return ((static_cast<index_type>(__strides[_Pos]) > static_cast<index_type>(0)) && ... && true);
            }
        }(make_index_sequence<__rank_>())),
            "layout_stride::mapping ctor: all strides must be greater than 0");
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__required_span_size_is_representable(__ext, __strides),
            "layout_stride::mapping ctor: required span size is not representable as index_type.");
        if constexpr (__rank_ > 1) {
            _LIBCPP_ASSERT_UNCATEGORIZED(([&]<size_t... _Pos>(index_sequence<_Pos...>) {
                // basically sort the dimensions based on strides and extents, sorting is represented in permute array
                array<rank_type, __rank_> __permute { _Pos... };
                __bubble_sort_by_strides(__permute);

                // check that this permutations represents a growing set
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
    _LIBCPP_HIDE_FROM_ABI constexpr mapping(
        const extents_type& __ext, const array<_OtherIndexType, __rank_>& __strides) noexcept
        : mapping(__ext, span(__strides)) {}

    template <class _StridedLayoutMapping>
        requires(__mdspan_detail::__layout_mapping_alike<_StridedLayoutMapping>
                    && is_constructible_v<extents_type, typename _StridedLayoutMapping::extents_type>
                    && _StridedLayoutMapping::is_always_unique() && _StridedLayoutMapping::is_always_strided())
    _LIBCPP_HIDE_FROM_ABI constexpr explicit(
        !(is_convertible_v<typename _StridedLayoutMapping::extents_type, extents_type>
            && (__mdspan_detail::__is_mapping_of<layout_left, _StridedLayoutMapping>
                || __mdspan_detail::__is_mapping_of<layout_right, _StridedLayoutMapping>
                || __mdspan_detail::__is_mapping_of<layout_stride, _StridedLayoutMapping>)))
        mapping(const _StridedLayoutMapping& __other) noexcept
        : __extents_(__other.extents())
        , __strides_([&]<size_t... _Pos>(index_sequence<_Pos...>) {
            // stride() only compiles for rank > 0
            if constexpr (__rank_ > 0) {
                return __mdspan_detail::__possibly_empty_array<index_type, __rank_> { static_cast<index_type>(
                    __other.stride(_Pos))... };
            } else {
                return __mdspan_detail::__possibly_empty_array<index_type, 0> {};
            }
        }(make_index_sequence<__rank_>())) {
        // stride() only compiles for rank > 0
        if constexpr (__rank_ > 0) {
            _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(([&]<size_t... _Pos>(index_sequence<_Pos...>) {
                return ((static_cast<index_type>(__other.stride(_Pos)) > static_cast<index_type>(0)) && ... && true);
            }(make_index_sequence<__rank_>())),
                "layout_stride::mapping converting ctor: all strides must be greater than 0");
        }
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(
            __mdspan_detail::__is_representable_as<index_type>(__other.required_span_size()),
            "layout_stride::mapping converting ctor: other.required_span_size() must be representable as index_type.");
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(static_cast<index_type>(0) == __offset(__other),
            "layout_stride::mapping converting ctor: base offset of mapping must be zero.");
    }

    _LIBCPP_HIDE_FROM_ABI constexpr mapping& operator=(const mapping&) noexcept = default;

    // [mdspan.layout.stride.obs], observers
    _LIBCPP_HIDE_FROM_ABI constexpr const extents_type& extents() const noexcept { return __extents_; }

    _LIBCPP_HIDE_FROM_ABI constexpr array<index_type, __rank_> strides() const noexcept {
        return [&]<size_t... _Pos>(index_sequence<_Pos...>) {
            return array<index_type, __rank_> { __strides_[_Pos]... };
        }(make_index_sequence<__rank_>());
    }

    _LIBCPP_HIDE_FROM_ABI constexpr index_type required_span_size() const noexcept {
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
    _LIBCPP_HIDE_FROM_ABI constexpr index_type operator()(_Indices... __idx) const noexcept {
        // Mappings are generally meant to be used for accessing allocations and are meant to guarantee to never
        // return a value exceeding required_span_size(), which is used to know how large an allocation one needs
        // Thus, this is a canonical point in multi-dimensional data structures to make invalid element access checks
        // However, mdspan does check this on its own, so for now we avoid double checking in hardened mode
        _LIBCPP_ASSERT_UNCATEGORIZED(__mdspan_detail::__is_multidimensional_index_in(__extents_, __idx...),
            "layout_stride::mapping: out of bounds indexing");
        return [&]<size_t... _Pos>(index_sequence<_Pos...>) {
            return ((static_cast<index_type>(__idx) * __strides_[_Pos]) + ... + index_type(0));
        }(make_index_sequence<sizeof...(_Indices)>());
    }

    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_unique() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_exhaustive() noexcept {
        if constexpr (__rank_ == 0) { return true; }
        for (size_t __r = 0; __r < __rank_; ++__r) {
            if (extents_type::static_extent(__r) == 0) { return true; }
        }
        return false;
    }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_strided() noexcept { return true; }

    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_unique() noexcept { return true; }
    _LIBCPP_HIDE_FROM_ABI constexpr bool is_exhaustive() const noexcept {
        if constexpr (is_always_exhaustive()) { return true; }
        index_type __span_size = required_span_size();
        if (__span_size == static_cast<index_type>(0)) { return true; }
        return __span_size == [&]<size_t... _Pos>(index_sequence<_Pos...>) {
            return (__extents_.extent(_Pos) * ... * static_cast<index_type>(1));
        }(make_index_sequence<__rank_>());
    }
    _LIBCPP_HIDE_FROM_ABI static constexpr bool is_strided() noexcept { return true; }

    // according to the standard layout_stride does not have a constraint on stride(r) for rank>0
    // it still has the precondition though
    _LIBCPP_HIDE_FROM_ABI constexpr index_type stride(rank_type __r) const noexcept {
        _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__r < __rank_, "layout_stride::mapping::stride(): invalid rank index");
        return __strides_[__r];
    }

    template <class _OtherMapping>
        requires(__mdspan_detail::__layout_mapping_alike<_OtherMapping>
            && (_OtherMapping::extents_type::rank() == __rank_) && _OtherMapping::is_always_strided())
    _LIBCPP_HIDE_FROM_ABI friend constexpr bool operator==(const mapping& __lhs, const _OtherMapping& __rhs) noexcept {
        if (__offset(__rhs)) { return false; }
        if constexpr (__rank_ == 0) {
            return true;
        } else {
            return __lhs.extents() == __rhs.extents() && [&]<size_t... _Pos>(index_sequence<_Pos...>) {
                // avoid warning when comparing signed and unsigner integers and pick the wider of two types
                using _CommonType = common_type_t<index_type, typename _OtherMapping::index_type>;
                return ((static_cast<_CommonType>(__lhs.stride(_Pos)) == static_cast<_CommonType>(__rhs.stride(_Pos)))
                    && ... && true);
            }(make_index_sequence<__rank_>());
        }
    }

private:
    _LIBCPP_NO_UNIQUE_ADDRESS extents_type __extents_ {};
    _LIBCPP_NO_UNIQUE_ADDRESS __mdspan_detail::__possibly_empty_array<index_type, __rank_> __strides_ {};
};

#endif // _LIBCPP_STD_VER >= 23

_LIBCPP_END_NAMESPACE_STD

_LIBCPP_POP_MACROS

#endif // _LIBCPP___MDSPAN_LAYOUT_STRIDE_H

// =============================================================================
// FILE: include/__mdspan/mdspan.h
// =============================================================================
// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
//===---------------------------------------------------------------------===//

#ifndef _LIBCPP___MDSPAN_MDSPAN_H
#define _LIBCPP___MDSPAN_MDSPAN_H

#include <array>
#include <span>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#pragma GCC system_header
#endif

_LIBCPP_PUSH_MACROS

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER >= 23

// Helper for lightweight test checking that one did pass a layout policy as LayoutPolicy template argument
namespace __mdspan_detail {
template <class _Layout, class _Extents>
concept __has_invalid_mapping = !requires { typename _Layout::template mapping<_Extents>; };
} // namespace __mdspan_detail

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
    using mapping_type     = layout_type::template mapping<extents_type>;
    using element_type     = _ElementType;
    using value_type       = remove_cv_t<element_type>;
    using index_type       = extents_type::index_type;
    using size_type        = extents_type::size_type;
    using rank_type        = extents_type::rank_type;
    using data_handle_type = accessor_type::data_handle_type;
    using reference        = accessor_type::reference;

    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr rank_type rank() noexcept { return extents_type::rank(); }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr rank_type rank_dynamic() noexcept {
        return extents_type::rank_dynamic();
    }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr size_t static_extent(rank_type __r) noexcept {
        return extents_type::static_extent(__r);
    }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr index_type extent(rank_type __r) const noexcept {
        return __map_.extents().extent(__r);
    }

    //--------------------------------------------------------------------------------
    // [mdspan.mdspan.cons], mdspan constructors, assignment, and destructor

    _LIBCPP_HIDE_FROM_ABI constexpr mdspan()
        requires((extents_type::rank_dynamic() > 0) && is_default_constructible_v<data_handle_type>
                    && is_default_constructible_v<mapping_type> && is_default_constructible_v<accessor_type>)
    = default;
    _LIBCPP_HIDE_FROM_ABI constexpr mdspan(const mdspan&) = default;
    _LIBCPP_HIDE_FROM_ABI constexpr mdspan(mdspan&&)      = default;

    template <class... _OtherIndexTypes>
        requires((is_convertible_v<_OtherIndexTypes, index_type> && ...)
                    && (is_nothrow_constructible_v<index_type, _OtherIndexTypes> && ...)
                    && ((sizeof...(_OtherIndexTypes) == rank()) || (sizeof...(_OtherIndexTypes) == rank_dynamic()))
                    && is_constructible_v<mapping_type, extents_type> && is_default_constructible_v<accessor_type>)
    _LIBCPP_HIDE_FROM_ABI explicit constexpr mdspan(data_handle_type __p, _OtherIndexTypes... __exts)
        : __ptr_(std::move(__p))
        , __map_(extents_type(static_cast<index_type>(std::move(__exts))...))
        , __acc_ {} {}

    template <class _OtherIndexType, size_t _Size>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
                    && is_nothrow_constructible_v<index_type, const _OtherIndexType&>
                    && ((_Size == rank()) || (_Size == rank_dynamic()))
                    && is_constructible_v<mapping_type, extents_type> && is_default_constructible_v<accessor_type>)
    explicit(_Size != rank_dynamic()) _LIBCPP_HIDE_FROM_ABI
        constexpr mdspan(data_handle_type __p, const array<_OtherIndexType, _Size>& __exts)
        : __ptr_(std::move(__p))
        , __map_(extents_type(__exts))
        , __acc_ {} {}

    template <class _OtherIndexType, size_t _Size>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
                    && is_nothrow_constructible_v<index_type, const _OtherIndexType&>
                    && ((_Size == rank()) || (_Size == rank_dynamic()))
                    && is_constructible_v<mapping_type, extents_type> && is_default_constructible_v<accessor_type>)
    explicit(_Size != rank_dynamic()) _LIBCPP_HIDE_FROM_ABI
        constexpr mdspan(data_handle_type __p, span<_OtherIndexType, _Size> __exts)
        : __ptr_(std::move(__p))
        , __map_(extents_type(__exts))
        , __acc_ {} {}

    _LIBCPP_HIDE_FROM_ABI constexpr mdspan(data_handle_type __p, const extents_type& __exts)
        requires(is_default_constructible_v<accessor_type> && is_constructible_v<mapping_type, const extents_type&>)
        : __ptr_(std::move(__p))
        , __map_(__exts)
        , __acc_ {} {}

    _LIBCPP_HIDE_FROM_ABI constexpr mdspan(data_handle_type __p, const mapping_type& __m)
        requires(is_default_constructible_v<accessor_type>)
        : __ptr_(std::move(__p))
        , __map_(__m)
        , __acc_ {} {}

    _LIBCPP_HIDE_FROM_ABI constexpr mdspan(data_handle_type __p, const mapping_type& __m, const accessor_type& __a)
        : __ptr_(std::move(__p))
        , __map_(__m)
        , __acc_(__a) {}

    template <class _OtherElementType, class _OtherExtents, class _OtherLayoutPolicy, class _OtherAccessor>
        requires(is_constructible_v<mapping_type, const typename _OtherLayoutPolicy::template mapping<_OtherExtents>&>
                    && is_constructible_v<accessor_type, const _OtherAccessor&>)
    explicit(!is_convertible_v<const typename _OtherLayoutPolicy::template mapping<_OtherExtents>&, mapping_type>
        || !is_convertible_v<const _OtherAccessor&, accessor_type>) _LIBCPP_HIDE_FROM_ABI
        constexpr mdspan(const mdspan<_OtherElementType, _OtherExtents, _OtherLayoutPolicy, _OtherAccessor>& __other)
        : __ptr_(__other.__ptr_)
        , __map_(__other.__map_)
        , __acc_(__other.__acc_) {
        static_assert(is_constructible_v<data_handle_type, const typename _OtherAccessor::data_handle_type&>,
            "mdspan: incompatible data_handle_type for mdspan construction");
        static_assert(
            is_constructible_v<extents_type, _OtherExtents>, "mdspan: incompatible extents for mdspan construction");

        // The following precondition is part of the standard, but is unlikely to be triggered.
        // The extents constructor checks this and the mapping must be storing the extents, since
        // its extents() function returns a const reference to extents_type.
        // The only way this can be triggered is if the mapping conversion constructor would for example
        // always construct its extents() only from the dynamic extents, instead of from the other extents.
        if constexpr (rank() > 0) {
            for (size_t __r = 0; __r < rank(); __r++) {
                // Not catching this could lead to out of bounds errors later
                // e.g. mdspan<int, dextents<char,1>, non_checking_layout> m =
                //        mdspan<int, dextents<unsigned, 1>, non_checking_layout>(ptr, 200); leads to an extent of -56
                //        on m
                _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS((static_extent(__r) == dynamic_extent)
                        || (static_cast<index_type>(__other.extent(__r))
                            == static_cast<index_type>(static_extent(__r))),
                    "mdspan: conversion mismatch of source dynamic extents with static extents");
            }
        }
    }

    _LIBCPP_HIDE_FROM_ABI constexpr mdspan& operator=(const mdspan&) = default;
    _LIBCPP_HIDE_FROM_ABI constexpr mdspan& operator=(mdspan&&)      = default;

    //--------------------------------------------------------------------------------
    // [mdspan.mdspan.members], members

    template <class... _OtherIndexTypes>
        requires((is_convertible_v<_OtherIndexTypes, index_type> && ...)
            && (is_nothrow_constructible_v<index_type, _OtherIndexTypes> && ...)
            && (sizeof...(_OtherIndexTypes) == rank()))
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr reference operator[](_OtherIndexTypes... __indices) const {
        return [&]<class... _IndexTypes>(_IndexTypes... __idxs) -> reference {
            _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS(__mdspan_detail::__is_multidimensional_index_in(extents(), __idxs...),
                "mdspan: operator[] out of bounds access");
            return __acc_.access(__ptr_, __map_(static_cast<index_type>(std::move(__idxs))...));
        }(extents_type::__index_cast(std::move(__indices))...);
    }

    template <class _OtherIndexType>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
            && is_nothrow_constructible_v<index_type, const _OtherIndexType&>)
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr reference operator[](
        const array<_OtherIndexType, rank()>& __indices) const {
        return __acc_.access(__ptr_, [&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            return __map_(extents_type::__index_cast(__indices[_Idxs])...);
        }(make_index_sequence<rank()>()));
    }

    template <class _OtherIndexType>
        requires(is_convertible_v<const _OtherIndexType&, index_type>
            && is_nothrow_constructible_v<index_type, const _OtherIndexType&>)
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr reference operator[](span<_OtherIndexType, rank()> __indices) const {
        return __acc_.access(__ptr_, [&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            return __map_(extents_type::__index_cast(__indices[_Idxs])...);
        }(make_index_sequence<rank()>()));
    }

    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr size_type size() const noexcept {
        // Could leave this as only checked in debug mode: semantically size() is never
        // guaranteed to be related to any accessible range
        _LIBCPP_ASSERT_UNCATEGORIZED(false == ([&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            size_type __prod = 1;
            return (__builtin_mul_overflow(__prod, extent(_Idxs), std::addressof(__prod)) || ... || false);
        }(make_index_sequence<rank()>())),
            "mdspan: size() is not representable as size_type");
        return [&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            return ((static_cast<size_type>(__map_.extents().extent(_Idxs))) * ... * size_type(1));
        }(make_index_sequence<rank()>());
    }

    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr bool empty() const noexcept {
        return [&]<size_t... _Idxs>(index_sequence<_Idxs...>) {
            return (rank() > 0) && ((__map_.extents().extent(_Idxs) == index_type(0)) || ... || false);
        }(make_index_sequence<rank()>());
    }

    _LIBCPP_HIDE_FROM_ABI friend constexpr void swap(mdspan& __x, mdspan& __y) noexcept {
        swap(__x.__ptr_, __y.__ptr_);
        swap(__x.__map_, __y.__map_);
        swap(__x.__acc_, __y.__acc_);
    }

    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr const extents_type& extents() const noexcept {
        return __map_.extents();
    }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr const data_handle_type& data_handle() const noexcept {
        return __ptr_;
    }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr const mapping_type& mapping() const noexcept { return __map_; }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr const accessor_type& accessor() const noexcept { return __acc_; }

    // per LWG-4021 "mdspan::is_always_meow() should be noexcept"
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_unique() noexcept {
        return mapping_type::is_always_unique();
    }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_exhaustive() noexcept {
        return mapping_type::is_always_exhaustive();
    }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr bool is_always_strided() noexcept {
        return mapping_type::is_always_strided();
    }

    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr bool is_unique() const { return __map_.is_unique(); }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr bool is_exhaustive() const { return __map_.is_exhaustive(); }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr bool is_strided() const { return __map_.is_strided(); }
    [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr index_type stride(rank_type __r) const { return __map_.stride(__r); }

private:
    _LIBCPP_NO_UNIQUE_ADDRESS data_handle_type __ptr_ {};
    _LIBCPP_NO_UNIQUE_ADDRESS mapping_type __map_ {};
    _LIBCPP_NO_UNIQUE_ADDRESS accessor_type __acc_ {};

    template <class, class, class, class> friend class mdspan;
};

#if _LIBCPP_STD_VER >= 26
template <class _ElementType, class... _OtherIndexTypes>
    requires((is_convertible_v<_OtherIndexTypes, size_t> && ...) && (sizeof...(_OtherIndexTypes) > 0))
explicit mdspan(_ElementType*, _OtherIndexTypes...)
    -> mdspan<_ElementType, extents<size_t, __maybe_static_ext<_OtherIndexTypes>...>>;
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

// This one is necessary because all the constructors take `data_handle_type`s, not
// `_ElementType*`s, and `data_handle_type` is taken from `accessor_type::data_handle_type`, which
// seems to throw off automatic deduction guides.
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

#endif // _LIBCPP_STD_VER >= 23

_LIBCPP_END_NAMESPACE_STD

_LIBCPP_POP_MACROS

// --- Cleanup compatibility layer shims to prevent polluting the preprocessor scope ---
#ifdef _STX_SHIM_BEGIN_NAMESPACE_STD
#undef _LIBCPP_BEGIN_NAMESPACE_STD
#undef _STX_SHIM_BEGIN_NAMESPACE_STD
#endif
#ifdef _STX_SHIM_END_NAMESPACE_STD
#undef _LIBCPP_END_NAMESPACE_STD
#undef _STX_SHIM_END_NAMESPACE_STD
#endif
#ifdef _STX_SHIM_HIDE_FROM_ABI
#undef _LIBCPP_HIDE_FROM_ABI
#undef _STX_SHIM_HIDE_FROM_ABI
#endif
#ifdef _STX_SHIM_NO_UNIQUE_ADDRESS
#undef _LIBCPP_NO_UNIQUE_ADDRESS
#undef _STX_SHIM_NO_UNIQUE_ADDRESS
#endif
#ifdef _STX_SHIM_NODEBUG
#undef _LIBCPP_NODEBUG
#undef _STX_SHIM_NODEBUG
#endif
#ifdef _STX_SHIM_ASSERT_VALID_ELEMENT_ACCESS
#undef _LIBCPP_ASSERT_VALID_ELEMENT_ACCESS
#undef _STX_SHIM_ASSERT_VALID_ELEMENT_ACCESS
#endif
#ifdef _STX_SHIM_ASSERT_UNCATEGORIZED
#undef _LIBCPP_ASSERT_UNCATEGORIZED
#undef _STX_SHIM_ASSERT_UNCATEGORIZED
#endif
#ifdef _STX_SHIM_PUSH_MACROS
#undef _LIBCPP_PUSH_MACROS
#undef _STX_SHIM_PUSH_MACROS
#endif
#ifdef _STX_SHIM_POP_MACROS
#undef _LIBCPP_POP_MACROS
#undef _STX_SHIM_POP_MACROS
#endif
#ifdef _STX_SHIM_STD_VER
#undef _LIBCPP_STD_VER
#undef _STX_SHIM_STD_VER
#endif
#ifdef _STX_SHIM_UNREACHABLE
#undef __libcpp_unreachable
#undef _STX_SHIM_UNREACHABLE
#endif

#endif // _LIBCPP___MDSPAN_MDSPAN_H

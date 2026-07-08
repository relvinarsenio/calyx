/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

#ifndef _STX_HIDE_FROM_ABI
#if defined(__GNUC__) || defined(__clang__)
#define _STX_HIDE_FROM_ABI __attribute__((__always_inline__)) inline
#else
#define _STX_HIDE_FROM_ABI inline
#endif
#endif

namespace stx {
namespace __tsc {

namespace chrono      = std::chrono;
namespace this_thread = std::this_thread;

/**
 * @brief Providing hardware-level hint for spin-waiting.
 */
_STX_HIDE_FROM_ABI void __cpu_pause() noexcept {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    this_thread::yield();
#endif
}

/**
 * @brief Read the hardware cycle counter (non-serializing).
 * @return 64-bit cycle count.
 */
[[nodiscard]] _STX_HIDE_FROM_ABI std::uint64_t __rdtsc() noexcept {
#if defined(__x86_64__)
    std::uint32_t __lo = 0, __hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(__lo), "=d"(__hi)::"memory");
    return (static_cast<std::uint64_t>(__hi) << 32) | static_cast<std::uint64_t>(__lo);
#elif defined(__aarch64__)
    std::uint64_t __val = 0;
    __asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(__val)::"memory");
    return __val;
#else
    return static_cast<std::uint64_t>(chrono::steady_clock::now().time_since_epoch().count());
#endif
}

/**
 * @brief Serialized start-of-interval cycle read.
 * @details Per Intel SDM Vol. 2 (RDTSC guidance): LFENCE before RDTSC
 * ensures all prior instructions and loads complete before the timestamp.
 * This is the recommended pattern for the start of a timed interval.
 */
[[nodiscard]] _STX_HIDE_FROM_ABI std::uint64_t __rdtsc_ordered() noexcept {
#if defined(__x86_64__)
    std::uint32_t __lo = 0, __hi = 0;
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(__lo), "=d"(__hi)::"memory");
    return (static_cast<std::uint64_t>(__hi) << 32) | static_cast<std::uint64_t>(__lo);
#elif defined(__aarch64__)
    std::uint64_t __val = 0;
    __asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(__val)::"memory");
    return __val;
#else
    return static_cast<std::uint64_t>(chrono::steady_clock::now().time_since_epoch().count());
#endif
}

/**
 * @brief Read the hardware cycle counter with partial serialization.
 * @details Per Intel SDM Vol. 2: RDTSCP waits for all prior instructions to
 * complete, but does NOT prevent subsequent instructions from starting before
 * the timestamp is read. Use rdtscp_ordered() when a full end-of-interval
 * barrier is required.
 * @return 64-bit cycle count.
 */
[[nodiscard]] _STX_HIDE_FROM_ABI std::uint64_t __rdtscp() noexcept {
#if defined(__x86_64__)
    std::uint32_t __lo = 0, __hi = 0;
    __asm__ __volatile__("rdtscp" : "=a"(__lo), "=d"(__hi)::"rcx", "memory");
    return (static_cast<std::uint64_t>(__hi) << 32) | static_cast<std::uint64_t>(__lo);
#elif defined(__aarch64__)
    std::uint64_t __val = 0;
    __asm__ __volatile__("isb; mrs %0, cntvct_el0; isb" : "=r"(__val)::"memory");
    return __val;
#else
    return __rdtsc();
#endif
}

/**
 * @brief Serialized end-of-interval cycle read.
 * @details Per Intel SDM Vol. 2: RDTSCP waits for all prior instructions,
 * then LFENCE prevents subsequent instructions from executing speculatively
 * before the timestamp is captured. This is the recommended pattern for
 * the end of a timed interval.
 */
[[nodiscard]] _STX_HIDE_FROM_ABI std::uint64_t __rdtscp_ordered() noexcept {
#if defined(__x86_64__)
    std::uint32_t __lo = 0, __hi = 0;
    __asm__ __volatile__("rdtscp\n\tlfence" : "=a"(__lo), "=d"(__hi)::"rcx", "memory");
    return (static_cast<std::uint64_t>(__hi) << 32) | static_cast<std::uint64_t>(__lo);
#elif defined(__aarch64__)
    std::uint64_t __val = 0;
    __asm__ __volatile__("isb; mrs %0, cntvct_el0; isb" : "=r"(__val)::"memory");
    return __val;
#else
    return __rdtsc();
#endif
}

/**
 * @brief Retrieve the hardware-reported timer frequency directly.
 * @details On x86_64, queries CPUID leaf 15H or 16H for nominal frequencies.
 * On AArch64, reads the system counter frequency register CNTFRQ_EL0.
 * @return Hardware clock frequency in cycles per nanosecond, or std::nullopt if unsupported.
 */
[[nodiscard]] _STX_HIDE_FROM_ABI std::optional<double> __hardware_frequency() noexcept {
#if defined(__x86_64__)
    std::uint32_t __eax = 0, __ebx = 0, __ecx = 0, __edx = 0;
    __asm__ __volatile__("cpuid" : "=a"(__eax), "=b"(__ebx), "=c"(__ecx), "=d"(__edx) : "a"(0), "c"(0));
    const std::uint32_t __max_leaf = __eax;
    if (__max_leaf < 0x15) { return std::nullopt; }

    __asm__ __volatile__("cpuid" : "=a"(__eax), "=b"(__ebx), "=c"(__ecx), "=d"(__edx) : "a"(0x15), "c"(0));

    if (__ebx == 0 || __eax == 0) { return std::nullopt; }

    if (__ecx != 0) { return (static_cast<double>(__ecx) * __ebx) / (static_cast<double>(__eax) * 1e9); }

    if (__max_leaf >= 0x16) {
        std::uint32_t __eax16 = 0, __ebx16 = 0, __ecx16 = 0, __edx16 = 0;
        __asm__ __volatile__("cpuid" : "=a"(__eax16), "=b"(__ebx16), "=c"(__ecx16), "=d"(__edx16) : "a"(0x16), "c"(0));
        if (__eax16 != 0) { return static_cast<double>(__eax16) / 1e3; }
    }

    return std::nullopt;
#elif defined(__aarch64__)
    std::uint64_t __freq = 0;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(__freq));
    if (__freq == 0) { return std::nullopt; }
    return static_cast<double>(__freq) / 1e9;
#else
    return std::nullopt;
#endif
}

/**
 * @brief Calibrate the cycle frequency against a steady clock.
 * @param __duration Duration to calibrate for (default 10ms).
 * @return Cycles per nanosecond.
 */
template <class _Rep, class _Period>
[[nodiscard]] _STX_HIDE_FROM_ABI double __calibrate(chrono::duration<_Rep, _Period> __duration) noexcept {
    if (__duration <= chrono::duration<_Rep, _Period>::zero()) { return __hardware_frequency().value_or(0.0); }
    const auto __t0 = chrono::steady_clock::now();
    const auto __c0 = __rdtsc_ordered();

    /** @brief Busy-wait for better precision than sleep. */
    while (chrono::steady_clock::now() - __t0 < __duration) {
        __cpu_pause();
    }

    const auto __t1 = chrono::steady_clock::now();
    const auto __c1 = __rdtscp_ordered();

    const auto __elapsed_ns = chrono::duration_cast<chrono::nanoseconds>(__t1 - __t0).count();
    if (__elapsed_ns <= 0) { return __hardware_frequency().value_or(0.0); }
    return static_cast<double>(__c1 - __c0) / static_cast<double>(__elapsed_ns);
}

} // namespace __tsc

namespace tsc {

_STX_HIDE_FROM_ABI void cpu_pause() noexcept {
    stx::__tsc::__cpu_pause();
}

[[nodiscard]] _STX_HIDE_FROM_ABI std::uint64_t rdtsc() noexcept {
    return stx::__tsc::__rdtsc();
}

[[nodiscard]] _STX_HIDE_FROM_ABI std::uint64_t rdtsc_ordered() noexcept {
    return stx::__tsc::__rdtsc_ordered();
}

[[nodiscard]] _STX_HIDE_FROM_ABI std::uint64_t rdtscp() noexcept {
    return stx::__tsc::__rdtscp();
}

[[nodiscard]] _STX_HIDE_FROM_ABI std::uint64_t rdtscp_ordered() noexcept {
    return stx::__tsc::__rdtscp_ordered();
}

template <class _Rep, class _Period>
[[nodiscard]] _STX_HIDE_FROM_ABI double calibrate(std::chrono::duration<_Rep, _Period> __duration) noexcept {
    return stx::__tsc::__calibrate(__duration);
}

[[nodiscard]] _STX_HIDE_FROM_ABI double calibrate(
    std::chrono::milliseconds __duration = std::chrono::milliseconds(10)) noexcept {
    return stx::__tsc::__calibrate(__duration);
}

} // namespace tsc
} // namespace stx

/** @brief Export to compatibility namespace alias to keep the rest of the project working without changes. */
namespace tsc = stx::tsc;

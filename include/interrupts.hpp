/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <array>
#include <atomic>
#include <csignal>
#include <mutex>
#include <ranges>
#include <type_traits>

/**
 * @file interrupts.hpp
 * @brief Async-signal-safe interrupt flag and thread-safe automatic signal handling.
 *
 * @details Provides a thread-safe and async-signal-safe mechanism to handle OS
 * interrupts (SIGINT, SIGTERM) gracefully across a multi-threaded application.
 * Compliant with POSIX.1-2024 by utilizing lock-free atomics.
 *
 * @note @c SA_RESTART is intentionally omitted from @c sa_flags. This ensures that
 * blocking syscalls (e.g., @c read, @c poll) fail immediately with @c EINTR,
 * allowing the application's event loops to detect the interrupt and shut down.
 */
namespace interrupt_state {

/**
 * @brief Concept ensuring a type yields a lock-free std::atomic.
 * @note Per C++ Core Guidelines T.10 and T.20, we constrain T to be trivially
 * copyable first to prevent hard compiler errors before evaluating lock-free status.
 */
template <typename T>
concept async_signal_safe = std::is_trivially_copyable_v<T> && std::atomic<T>::is_always_lock_free;

/**
 * @brief Atomic type guaranteed to be async-signal-safe per POSIX.1-2024.
 * @note Per C++ Core Guidelines T.42, we use a template alias to enforce
 * the concept constraint implicitly and simplify notation.
 */
template <async_signal_safe T> using signal_safe_atomic = std::atomic<T>;

/** @brief Global interrupt flag, written by the signal handler, polled by the app. */
inline signal_safe_atomic<bool> g_interrupted { false };

} // namespace interrupt_state

namespace interrupts_impl {

/** @brief Signals to handle. Extend this array to add more. */
inline constexpr std::array kHandledSignals { SIGINT, SIGTERM };

/** @brief Minimal async-signal-safe OS handler. */
extern "C" inline void signal_handler(int /*signum*/) noexcept {
    /** @note Relaxed ordering is sufficient here. This flag is a one-way latch with no dependent memory stores. */
    interrupt_state::g_interrupted.store(true, std::memory_order_relaxed);
}

/** @brief Constructs a sigaction configured for our handler. */
inline auto make_handler_action() noexcept {
    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return sa;
}

/** @brief Installs signal_handler for all kHandledSignals. */
inline void install_signal_handler() noexcept {
    auto sa = make_handler_action();
    for (auto sig : kHandledSignals) {
        sigaction(sig, &sa, nullptr);
    }
}

/** @brief Overload capturing previous dispositions for later restore. */
inline void install_signal_handler(std::array<struct sigaction, kHandledSignals.size()>& old) noexcept {
    auto sa = make_handler_action();
    for (auto&& [sig, prev] : std::views::zip(kHandledSignals, old)) {
        sigaction(sig, &sa, &prev);
    }
}

/** @brief Scoped RAII guard that captures and restores previous signal dispositions. */
class SignalGuard {
    std::array<struct sigaction, kHandledSignals.size()> prev_ {};

public:
    SignalGuard() noexcept { install_signal_handler(prev_); }

    ~SignalGuard() noexcept {
        for (auto&& [sig, prev] : std::views::zip(kHandledSignals, prev_)) {
            sigaction(sig, &prev, nullptr);
        }
    }

    SignalGuard(const SignalGuard&)            = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;
    SignalGuard(SignalGuard&&)                 = delete;
    SignalGuard& operator=(SignalGuard&&)      = delete;
};

/**
 * @brief Thread-safe, idempotent process-wide signal handler registration.
 * @note Uses std::call_once (not static SignalGuard) to avoid restoration during
 * static destruction. noexcept is intentional — OS futex failure is unrecoverable.
 */
inline void ensure_signal_handler_installed() noexcept {
    static std::once_flag flag;
    std::call_once(flag, []() noexcept { install_signal_handler(); });
}

} // namespace interrupts_impl

/** @brief Explicitly initialize process-wide signal handlers (optional, for early startup). */
inline void init_signal_handlers() noexcept {
    interrupts_impl::ensure_signal_handler_installed();
}

/** @brief Polls global interrupt flag (auto-installs handlers if needed). */
[[nodiscard]] inline bool check_interrupted() noexcept {
    interrupts_impl::ensure_signal_handler_installed();
    return interrupt_state::g_interrupted.load(std::memory_order_relaxed);
}

/** @brief Programmatically raise the interrupt flag without an OS signal. */
inline void trigger_interrupt() noexcept {
    interrupts_impl::ensure_signal_handler_installed();
    interrupt_state::g_interrupted.store(true, std::memory_order_relaxed);
}

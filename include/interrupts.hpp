/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <csignal>

/**
 * @file interrupts.hpp
 * @brief Async-signal-safe interrupt flag and RAII signal guard.
 *
 * @details Compliant with ISO C 2024 §7.14.1.1 and POSIX.1-2024 signal-safety requirements.
 *
 * The interrupt flag uses @c volatile @c std::sig_atomic_t — the only type formally
 * guaranteed by both ISO C and POSIX to be safely readable and writable from within
 * a signal handler without undefined behavior. While @c std::atomic<bool> is lock-free
 * on all mainstream platforms, using it inside a signal handler is only conditionally
 * valid per the C++ standard (requires @c is_always_lock_free to be true).
 *
 * @note @c SA_RESTART is intentionally omitted from @c sa_flags. This causes blocking
 * syscalls (e.g., @c read, @c write, @c poll) to fail with @c EINTR when a signal is
 * delivered, allowing the application's EINTR retry loops (@ref posix::eintr_loop) to
 * detect the interrupt flag and break out cleanly. Setting @c SA_RESTART would silently
 * resume those syscalls, defeating graceful cancellation.
 */
namespace interrupt_state {

/**
 * @brief Global interrupt flag, written by signal handler, polled by application loops.
 *
 * @note @c volatile ensures the compiler re-reads the variable on each access.
 * @c sig_atomic_t guarantees atomic store/load even from an async signal context.
 */
inline volatile std::sig_atomic_t g_interrupted = 0;

} // namespace interrupt_state

/**
 * @brief Minimal async-signal-safe handler that sets the interrupt flag.
 *
 * @note Per POSIX.1-2024, only async-signal-safe operations are permitted here.
 * Writing to a @c volatile @c sig_atomic_t is the canonical safe pattern.
 */
extern "C" inline void signal_handler(int /*signum*/) noexcept {
    interrupt_state::g_interrupted = 1;
}

/** @brief Poll the global interrupt flag (non-blocking). */
inline constexpr auto check_interrupted = []() noexcept -> bool { return interrupt_state::g_interrupted != 0; };

/** @brief Programmatically raise the interrupt flag without delivering a signal. */
inline constexpr auto trigger_interrupt = []() noexcept { interrupt_state::g_interrupted = 1; };

/**
 * @brief RAII guard that installs signal handlers on construction and restores originals on destruction.
 *
 * @details Captures the previous @c SIGINT and @c SIGTERM dispositions via the @c oldact
 * output parameter of @c sigaction(2), and restores them when the guard goes out of scope.
 * This ensures that nested or library-installed handlers are preserved correctly.
 */
class SignalGuard {
    struct sigaction prev_sigint_ {};
    struct sigaction prev_sigterm_ {};

public:
    SignalGuard() noexcept {
        struct sigaction sa {};
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);
        /** @note sa_flags = 0: intentionally no SA_RESTART, see file-level documentation. */
        sa.sa_flags = 0;

        sigaction(SIGINT, &sa, &prev_sigint_);
        sigaction(SIGTERM, &sa, &prev_sigterm_);
    }

    ~SignalGuard() noexcept {
        sigaction(SIGINT, &prev_sigint_, nullptr);
        sigaction(SIGTERM, &prev_sigterm_, nullptr);
    }

    SignalGuard(const SignalGuard&)            = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;
    SignalGuard(SignalGuard&&)                 = delete;
    SignalGuard& operator=(SignalGuard&&)      = delete;
};
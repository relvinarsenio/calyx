/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <atomic>
#include <csignal>

namespace interrupt_state {
inline std::atomic<bool> g_internal_state { false };
}

extern "C" inline void signal_handler(int) noexcept {
    interrupt_state::g_internal_state.store(true, std::memory_order_relaxed);
}

inline constexpr auto check_interrupted
    = []() noexcept -> bool { return interrupt_state::g_internal_state.load(std::memory_order_relaxed); };

inline constexpr auto trigger_interrupt
    = []() noexcept { interrupt_state::g_internal_state.store(true, std::memory_order_relaxed); };

class SignalGuard {
public:
    inline SignalGuard() {
        struct sigaction sa = {};
        sa.sa_handler       = signal_handler;
        sigemptyset(&sa.sa_mask);

        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }
    ~SignalGuard() = default;

    SignalGuard(const SignalGuard&)            = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;
    SignalGuard(SignalGuard&&)                 = delete;
    SignalGuard& operator=(SignalGuard&&)      = delete;
};
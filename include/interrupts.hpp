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
#include <concepts>
#include <csignal>
#include <cstddef>
#include <ranges>
#include <type_traits>
#include <utility>

/**
 * @file interrupts.hpp
 * @brief Thread-safe and async-signal-safe POSIX signal handling infrastructure.
 *
 * @details Provides an intrusive linked-list RAII controller (PosixSignalController) for managing
 * process-wide OS signals (SIGINT, SIGTERM). Compliant with POSIX.1-2024 using lock-free atomics
 * and move-only ownership transfer across nested scopes.
 *
 * @note Overkill for Calyx's single-instance use case, built for fun/practice — API stays simple
 * either way.
 */

/**
 * @brief Concept defining the required public interface contract for signal controllers.
 */
template <typename T>
concept SignalControllerType = requires(T t, const T ct) {
    t.install();
    t.restore();
    { ct.is_interrupted() } -> std::convertible_to<bool>;
    t.trigger();
    t.reset();
};

/**
 * @brief Concept ensuring a type is async-signal-safe for atomic operations.
 * @note Constrained per C++ Core Guidelines T.10 to trivially copyable/destructible types.
 */
template <typename T>
concept AsyncSignalSafe
    = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> && std::atomic<T>::is_always_lock_free;

/**
 * @brief Template alias enforcing async-signal-safe atomic types per POSIX.1-2024 and C++ Core Guidelines T.42.
 */
template <AsyncSignalSafe T> using signal_safe_atomic = std::atomic<T>;

/**
 * @brief RAII controller managing nested POSIX signal handlers via an intrusive doubly-linked list.
 * @details Transfers OS handler ownership O(1) on out-of-order destruction, eliminating redundant syscalls.
 * @note Thread Safety: Status polling and signal delivery are lock-free and thread-safe.
 * Concurrent construction/destruction across threads requires external synchronization as intrusive list
 * pointer mutations (prev_, next_) are not atomic.
 */
class PosixSignalController {
public:
    /** @brief OS signals registered and handled by this controller. */
    static constexpr std::array kHandledSignals { SIGINT, SIGTERM };

    /**
     * @brief Constructs and registers the controller instance into the intrusive stack.
     */
    PosixSignalController() noexcept {
        prev_ = active_instance_.exchange(this, std::memory_order_relaxed);
        if (prev_) {
            prev_->next_ = this;
        } else {
            install_root();
        }
    }

    /**
     * @brief Destroys the controller instance, unstacking or transferring OS handler ownership.
     */
    ~PosixSignalController() noexcept {
        PosixSignalController* expected = this;
        active_instance_.compare_exchange_strong(expected, prev_, std::memory_order_relaxed);

        if (next_) { next_->prev_ = prev_; }
        if (prev_) { prev_->next_ = next_; }

        if (installed_) {
            if (auto* const survivor = find_survivor()) {
                transfer_ownership_to(survivor);
            } else {
                restore();
            }
        }
    }

    /**
     * @brief Programmatically triggers interrupt flag on the active top-level instance.
     */
    static void trigger_active() noexcept {
        auto* ptr = active_instance_.load(std::memory_order_relaxed);
        if (ptr) { ptr->trigger(); }
    }

    /**
     * @brief Checks whether the active top-level instance has been interrupted.
     * @return True if the active instance is interrupted, false otherwise.
     */
    [[nodiscard]] static bool check_active() noexcept {
        const auto* ptr = active_instance_.load(std::memory_order_relaxed);
        return ptr ? ptr->is_interrupted() : false;
    }

    /**
     * @brief Registers POSIX signal handlers with the OS.
     * @note SA_RESTART is intentionally omitted from sa_flags. This ensures blocking syscalls
     * (e.g., read, poll) fail immediately with EINTR, allowing application loops to detect signals cleanly.
     */
    void install() noexcept {
        if (installed_) { return; }

        struct sigaction sa {};
        sa.sa_handler = &PosixSignalController::signal_trampoline;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        for (auto&& [idx, sig, old_act] : std::views::zip(std::views::iota(0u), kHandledSignals, old_actions_)) {
            if (sigaction(sig, &sa, &old_act) != 0) {
                rollback(idx);
                return;
            }
        }
        installed_ = true;
    }

    /**
     * @brief Restores original POSIX signal dispositions to the OS.
     */
    void restore() noexcept {
        if (!installed_) { return; }
        rollback(kHandledSignals.size());
        installed_ = false;
    }

    /**
     * @brief Returns whether this specific controller instance has been interrupted.
     */
    [[nodiscard]] bool is_interrupted() const noexcept { return interrupted_.load(std::memory_order_relaxed); }

    /**
     * @brief Sets the interrupt flag on this specific controller instance.
     */
    void trigger() noexcept { interrupted_.store(true, std::memory_order_relaxed); }

    /**
     * @brief Resets the interrupt flag on this specific controller instance.
     */
    void reset() noexcept { interrupted_.store(false, std::memory_order_relaxed); }

    PosixSignalController(const PosixSignalController&)            = delete;
    PosixSignalController& operator=(const PosixSignalController&) = delete;
    PosixSignalController(PosixSignalController&&)                 = delete;
    PosixSignalController& operator=(PosixSignalController&&)      = delete;

private:
    void install_root() noexcept {
        install();
        if (!installed_) {
            PosixSignalController* expected = this;
            active_instance_.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
        }
    }

    void rollback(std::size_t count) noexcept {
        for (auto&& [sig, old_act] : std::views::zip(kHandledSignals, old_actions_) | std::views::take(count)) {
            sigaction(sig, &old_act, nullptr);
        }
    }

    [[nodiscard]] PosixSignalController* find_survivor() noexcept {
        if (next_) { return next_; }
        if (prev_) { return prev_->find_root(); }
        return nullptr;
    }

    [[nodiscard]] PosixSignalController* find_root() noexcept {
        auto* curr = this;
        while (curr->prev_) {
            curr = curr->prev_;
        }
        return curr;
    }

    void transfer_ownership_to(PosixSignalController* target) noexcept {
        if (!target) { return; }
        target->installed_   = true;
        target->old_actions_ = std::move(old_actions_);
        installed_           = false;
    }

    static void signal_trampoline(int) noexcept { trigger_active(); }

    PosixSignalController* prev_ { nullptr };
    PosixSignalController* next_ { nullptr };
    static inline signal_safe_atomic<PosixSignalController*> active_instance_ { nullptr };
    signal_safe_atomic<bool> interrupted_ { false };
    bool installed_ { false };
    std::array<struct sigaction, kHandledSignals.size()> old_actions_ {};
};

/**
 * @brief Polls whether the active signal controller has received an interrupt.
 * @return True if an interrupt signal was received or triggered, false otherwise.
 */
[[nodiscard]] inline bool check_interrupted() noexcept {
    return PosixSignalController::check_active();
}

/**
 * @brief Programmatically triggers an interrupt on the active signal controller.
 */
inline void trigger_interrupt() noexcept {
    PosixSignalController::trigger_active();
}

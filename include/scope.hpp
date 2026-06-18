/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <type_traits>
#include <utility>

/**
 * @brief RAII scope guard (C++ Library Fundamentals TS - scope_exit).
 * @details Executes cleanup on destruction unless released. Movable, non-copyable.
 * @tparam F Callable type (Destructible, Invocable).
 */
template <typename F>
    requires std::invocable<F&> && std::destructible<F>
class [[nodiscard]] scope_exit {
    [[no_unique_address]] F func_;
    bool active_ { true };

public:
    /**
     * @brief Constructs a scope guard with copy/fallback behavior for potentially throwing callables.
     * @details Guarantees that the cleanup callback is executed immediately if the construction
     *          of the internal callable copy throws, preventing resource leaks.
     */
    template <typename EFP>
        requires(!std::is_same_v<std::remove_cvref_t<EFP>, scope_exit>) && std::is_constructible_v<F, EFP>
        && (std::is_lvalue_reference_v<EFP> || !std::is_nothrow_constructible_v<F, EFP>)
    explicit scope_exit(EFP&& f) noexcept(std::is_nothrow_constructible_v<F, EFP&>)
#ifdef __cpp_exceptions
        try
#endif
        : func_(f) {
    }
#ifdef __cpp_exceptions
    catch (...) {
        f();
    }
#endif

    /**
     * @brief Constructs a scope guard with zero-overhead forwarding when nothrow guarantees are met.
     * @details Bypasses copy-construction overhead by forwarding the rvalue directly.
     */
    template <typename EFP>
        requires(!std::is_same_v<std::remove_cvref_t<EFP>, scope_exit>) && std::is_constructible_v<F, EFP>
        && (!std::is_lvalue_reference_v<EFP>) && std::is_nothrow_constructible_v<F, EFP>
    explicit scope_exit(EFP&& f) noexcept
        : func_(std::forward<EFP>(f)) {}

    /**
     * @brief Destructor that triggers the registered cleanup action.
     * @details The cleanup callable is executed if the guard remains active.
     */
    ~scope_exit() noexcept {
        if (active_) { func_(); }
    }

    scope_exit(const scope_exit&)            = delete;
    scope_exit& operator=(const scope_exit&) = delete;

    /**
     * @brief Transfers cleanup responsibility using no-throw move construction.
     * @details Ensures that ownership is transferred safely without throwing exceptions,
     *          rendering the source guard inactive.
     */
    scope_exit(scope_exit&& other) noexcept
        requires std::is_nothrow_move_constructible_v<F>
        : func_(std::move(other.func_))
        , active_(std::exchange(other.active_, false)) {}

    /**
     * @brief Transfers cleanup responsibility via copying when the move constructor might throw.
     * @details Fallback mechanism to preserve the noexcept guarantee of the move operation,
     *          preventing exception leaks during state transfer.
     */
    scope_exit(scope_exit&& other) noexcept(std::is_nothrow_copy_constructible_v<F>)
        requires(!std::is_nothrow_move_constructible_v<F>) && std::is_copy_constructible_v<F>
        : func_(other.func_)
        , active_(std::exchange(other.active_, false)) {}

    scope_exit& operator=(scope_exit&&) = delete;

    /**
     * @brief Disables the execution of the cleanup action on destruction.
     * @details Used when the guarded resource has been successfully committed or transferred.
     */
    void release() noexcept { active_ = false; }
};

template <typename F>
    requires std::invocable<F&> && std::destructible<F>
scope_exit(F) -> scope_exit<F>;

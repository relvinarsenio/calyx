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
template <typename F> class [[nodiscard]] scope_exit {
    [[no_unique_address]] F func_;
    bool active_ { true };

public:
    /** @brief Safety path: copy 'f' so it remains valid if initialization throws. */
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
    } // Call fallback on construction failure.
#endif

    /** @brief Fast path: move 'f' if it is an rvalue and move is noexcept. */
    template <typename EFP>
        requires(!std::is_same_v<std::remove_cvref_t<EFP>, scope_exit>) && std::is_constructible_v<F, EFP>
        && (!std::is_lvalue_reference_v<EFP>) && std::is_nothrow_constructible_v<F, EFP>
    explicit scope_exit(EFP&& f) noexcept
        : func_(std::forward<EFP>(f)) {}

    /** @brief Triggers cleanup if active. std::terminate() on throw. */
    ~scope_exit() noexcept {
        if (active_) { func_(); }
    }

    scope_exit(const scope_exit&)            = delete;
    scope_exit& operator=(const scope_exit&) = delete;

    /** @brief Transfers cleanup responsibility to this guard. */
    scope_exit(scope_exit&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
        : func_(std::move(other.func_))
        , active_(std::exchange(other.active_, false)) {}

    scope_exit& operator=(scope_exit&&) = delete;

    /** @brief Disables the guard. */
    void release() noexcept { active_ = false; }
};

template <typename F> scope_exit(F&&) -> scope_exit<std::decay_t<F>>;

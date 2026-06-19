/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

/**
 * @brief RAII scope guard (ISO/IEC TS 19568 - scope_exit, P0052R10).
 * @details Executes cleanup on destruction unless released. Movable, non-copyable.
 * @tparam F Callable type. Must satisfy Destructible and be invocable as lvalue.
 */
template <typename F>
    requires std::invocable<F&> && std::destructible<F>
class [[nodiscard]] scope_exit {
    [[no_unique_address]] F func_;
    bool active_ { true };

public:
    /**
     * @brief Copy-initializes the exit function; calls f() immediately if initialization throws.
     * @details Active when EFP is an lvalue reference OR construction from EFP is not noexcept.
     *          The function-try-block ensures the callable is invoked on construction failure.
     */
    template <typename EFP>
        requires(!std::same_as<std::remove_cvref_t<EFP>, scope_exit>) && std::constructible_from<F, EFP>
        && (std::is_lvalue_reference_v<EFP> || !std::is_nothrow_constructible_v<F, EFP>)
    [[nodiscard]] explicit scope_exit(EFP&& f) noexcept(std::is_nothrow_constructible_v<F, EFP&>)
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
     * @brief Forward-initializes the exit function with zero-overhead when noexcept is guaranteed.
     * @details Active when EFP is an rvalue AND construction from EFP is noexcept, allowing
     *          a direct move instead of a defensive copy (P0052R10 §8).
     */
    template <typename EFP>
        requires(!std::same_as<std::remove_cvref_t<EFP>, scope_exit>) && std::constructible_from<F, EFP>
        && (!std::is_lvalue_reference_v<EFP>) && std::is_nothrow_constructible_v<F, EFP>
    explicit scope_exit(EFP&& f) noexcept
        : func_(std::forward<EFP>(f)) {}

    /**
     * @brief Executes the registered cleanup callable if the guard is still active.
     */
    constexpr ~scope_exit() noexcept {
        if (active_) { func_(); }
    }

    scope_exit(const scope_exit&)            = delete;
    scope_exit& operator=(const scope_exit&) = delete;
    scope_exit& operator=(scope_exit&&)      = delete;

    /**
     * @brief Move-constructs via forward when F is noexcept-move-constructible (P0052R10 §22).
     * @details Deactivates the source guard unconditionally; construction is guaranteed noexcept.
     */
    scope_exit(scope_exit&& other) noexcept
        requires std::is_nothrow_move_constructible_v<F>
        : func_(std::move(other.func_))
        , active_(std::exchange(other.active_, false)) {}

    /**
     * @brief Move-constructs via copy when F cannot be noexcept-moved (P0052R10 §22).
     * @details Falls back to copy construction to preserve the strong exception guarantee.
     *          Uses is_copy_constructible_v (not std::copy_constructible) to match R10 §21
     *          exactly and avoid over-constraining types with deleted move constructors.
     */
    scope_exit(scope_exit&& other) noexcept(std::is_nothrow_copy_constructible_v<F>)
        requires(!std::is_nothrow_move_constructible_v<F>) && std::is_copy_constructible_v<F>
        : func_(other.func_)
        , active_(std::exchange(other.active_, false)) {}

    /**
     * @brief Deactivates the guard, preventing execution of the cleanup callable.
     * @details Call when the guarded operation has completed successfully and
     *          cleanup is no longer needed.
     */
    constexpr void release() noexcept { active_ = false; }
};

template <typename F>
    requires std::invocable<F&> && std::destructible<F>
scope_exit(F) -> scope_exit<F>;

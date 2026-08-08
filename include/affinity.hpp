/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "config.hpp"
#include "posix.hpp"
#include "posix_error.hpp"
#include "scope.hpp"
#include "utils.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <pthread.h>
#include <ranges>
#include <sched.h>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

/**
 * @file affinity.hpp
 * @brief RAII CPU core isolation and worker thread affinity management.
 *
 * Provides single-core thread pinning with deterministic restoration on scope exit,
 * alongside factory helpers for isolated object construction.
 */
namespace affinity {

struct IsolationError {
    std::error_code ec {};
    std::string_view context {};
};

enum class IsolationState : std::uint8_t {
    NotIsolated,
    Isolated,
};

/**
 * @brief Dynamic RAII wrapper for POSIX cpu_set_t masks.
 */
class CpuSet {
public:
    constexpr CpuSet() noexcept = default;
    ~CpuSet() noexcept          = default;

    CpuSet(const CpuSet&)            = delete;
    CpuSet& operator=(const CpuSet&) = delete;

    constexpr CpuSet(CpuSet&& other) noexcept
        : ptr_(std::move(other.ptr_))
        , mask_size_(std::exchange(other.mask_size_, 0uz))
        , num_cpus_(std::exchange(other.num_cpus_, 0u)) {}

    constexpr CpuSet& operator=(CpuSet&& other) noexcept {
        if (this != &other) [[likely]] {
            ptr_       = std::move(other.ptr_);
            mask_size_ = std::exchange(other.mask_size_, 0uz);
            num_cpus_  = std::exchange(other.num_cpus_, 0u);
        }
        return *this;
    }

    [[nodiscard]] static std::expected<CpuSet, std::error_code> allocate(std::uint32_t num_cpus) noexcept {
        if (num_cpus == 0u || num_cpus > toUInt(std::numeric_limits<int>::max())) [[unlikely]] {
            return std::unexpected(posix::make_error(EINVAL));
        }

        const auto count { toInt(num_cpus) };
        const std::size_t mask_size { CPU_ALLOC_SIZE(count) };
        const auto raw { posix::expect_result<posix::error_style::pointer>(CPU_ALLOC(count)) };
        if (!raw) [[unlikely]] { return std::unexpected(raw.error()); }

        CpuSet set { *raw, mask_size, num_cpus };
        set.zero();
        return set;
    }

    void zero() noexcept {
        if (ptr_) [[likely]] { CPU_ZERO_S(mask_size_, ptr_.get()); }
    }

    bool set(std::uint32_t cpu) noexcept {
        if (ptr_ && cpu < capacity_bits()) [[likely]] {
            CPU_SET_S(cpu, mask_size_, ptr_.get());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool is_set(std::uint32_t cpu) const noexcept {
        if (ptr_ && cpu < capacity_bits()) [[likely]] {
            return CPU_ISSET_S(cpu, mask_size_, const_cast<cpu_set_t*>(ptr_.get())) != 0;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::uint32_t> first_set_cpu() const noexcept {
        if (!ptr_) [[unlikely]] { return std::nullopt; }
        const auto total_cpus { num_cpus_ > 0u ? num_cpus_ : toUInt(capacity_bits()) };
        const auto cpus_range { std::views::iota(0u, total_cpus) };
        const auto match { std::ranges::find_if(
            cpus_range, [this](std::uint32_t cpu) noexcept { return is_set(cpu); }) };
        if (match != cpus_range.end()) { return *match; }
        return std::nullopt;
    }

    [[nodiscard]] cpu_set_t* get() noexcept { return ptr_.get(); }
    [[nodiscard]] const cpu_set_t* get() const noexcept { return ptr_.get(); }
    [[nodiscard]] std::size_t size_bytes() const noexcept { return mask_size_; }
    [[nodiscard]] std::size_t capacity_bits() const noexcept { return safe_mul(mask_size_, 8uz).value_or(mask_size_); }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    struct CpuSetDeleter {
        void operator()(cpu_set_t* p) const noexcept {
            if (p) [[likely]] { CPU_FREE(p); }
        }
    };

    constexpr CpuSet(cpu_set_t* raw, std::size_t size, std::uint32_t cpus) noexcept
        : ptr_(raw)
        , mask_size_(size)
        , num_cpus_(cpus) {}

    std::unique_ptr<cpu_set_t, CpuSetDeleter> ptr_ { nullptr };
    std::size_t mask_size_ { 0uz };
    std::uint32_t num_cpus_ { 0u };
};

/**
 * @brief Callback signature for synchronizing kernel worker pools with pinned affinity.
 */
using WorkerSyncCallback = std::move_only_function<std::expected<void, IsolationError>(
    const cpu_set_t* mask, std::size_t mask_size, std::uint32_t num_cpus, std::int32_t target_cpu) const noexcept>;

template <typename T> struct is_variant : std::false_type {};
template <typename... Ts> struct is_variant<std::variant<Ts...>> : std::true_type {};
template <typename T> inline constexpr bool is_variant_v = is_variant<T>::value;

template <typename T> struct FactoryResultTraits {
    using value_type                  = T;
    using error_type                  = void;
    static constexpr bool is_expected = false;
};

template <typename T, typename E> struct FactoryResultTraits<std::expected<T, E>> {
    using value_type                  = T;
    using error_type                  = E;
    static constexpr bool is_expected = true;
};

template <typename R>
concept ValidFactoryResult = !std::same_as<std::remove_cvref_t<R>, void>;

template <typename F>
concept IsolatedFactory = std::invocable<F> && ValidFactoryResult<std::invoke_result_t<F>>;

template <typename T>
concept HasSetWorkerAffinity
    = requires(T& obj, const cpu_set_t* mask, std::size_t mask_size, std::uint32_t num_cpus, std::int32_t target_cpu) {
          { obj.set_worker_affinity(mask, mask_size, num_cpus, target_cpu) };
      };

template <typename T>
concept HasRegisterWorkerAffinity = requires(T& obj, const cpu_set_t* mask, std::size_t mask_size) {
    { obj.register_worker_affinity(mask, mask_size) };
};

template <typename T>
concept WorkerSyncObject = HasSetWorkerAffinity<T> || HasRegisterWorkerAffinity<T>;

template <typename E>
concept ErrorType
    = std::convertible_to<E, std::error_code> || std::convertible_to<E, IsolationError> || requires(const E& e) {
          { e.ec } -> std::convertible_to<std::error_code>;
      } || std::is_integral_v<E> || std::is_enum_v<E> || is_variant_v<E>;

/** @brief Superset of ErrorType covering fallback error types (e.g. empty sentinel structs) with no extractable code.
 */
template <typename E>
concept AnyErrorSource = ErrorType<E> || std::is_empty_v<E>;

template <typename T, typename... Args>
concept HasStaticCreate = requires(Args&&... args) {
    { T::create(std::forward<Args>(args)...) };
};

template <typename T, typename... Args>
concept FactoryCreatable = HasStaticCreate<T, Args...> || std::is_constructible_v<T, Args...>;

/**
 * @brief Scope-based CPU affinity isolation controller.
 *
 * Provides CPU core pinning guarantees for high-performance execution threads
 * with RAII lifetime management.
 *
 * @note Thread affinity operations apply exclusively to the calling thread.
 * Construction and destruction MUST occur on the exact same thread.
 */
class [[nodiscard]] CoreAffinityGuard {
public:
    constexpr CoreAffinityGuard() noexcept = default;
    ~CoreAffinityGuard() noexcept { restore(); }

    CoreAffinityGuard(const CoreAffinityGuard&)            = delete;
    CoreAffinityGuard& operator=(const CoreAffinityGuard&) = delete;

    constexpr CoreAffinityGuard(CoreAffinityGuard&& other) noexcept
        : original_mask_(std::move(other.original_mask_))
        , target_cpu_(std::exchange(other.target_cpu_, -1))
        , num_cpus_(std::exchange(other.num_cpus_, 0u))
        , state_(std::exchange(other.state_, IsolationState::NotIsolated)) {}

    constexpr CoreAffinityGuard& operator=(CoreAffinityGuard&& other) noexcept {
        if (this != &other) [[likely]] {
            restore();
            original_mask_ = std::move(other.original_mask_);
            target_cpu_    = std::exchange(other.target_cpu_, -1);
            num_cpus_      = std::exchange(other.num_cpus_, 0u);
            state_         = std::exchange(other.state_, IsolationState::NotIsolated);
        }
        return *this;
    }

    /**
     * @brief Constructs an isolated object pinned to the active CPU core.
     * @param args Arguments for target object construction.
     * @return Guard and constructed object pair, or isolation error.
     */
    template <typename T, typename... Args>
        requires FactoryCreatable<T, Args...>
    [[nodiscard]] static auto make_isolated(Args&&... args) noexcept
        -> std::expected<std::pair<CoreAffinityGuard, T>, IsolationError> {
        return make_isolated_at<T>(config::kAffinityAutoCore, std::forward<Args>(args)...);
    }

    /**
     * @brief Constructs an isolated object pinned to a specified CPU core.
     * @param target_cpu Target core index.
     * @param args Arguments for target object construction.
     * @return Guard and constructed object pair, or isolation error.
     */
    template <typename T, typename... Args>
        requires FactoryCreatable<T, Args...>
    [[nodiscard]] static auto make_isolated_at(std::int32_t target_cpu, Args&&... args)
        -> std::expected<std::pair<CoreAffinityGuard, T>, IsolationError> {
        return make_isolated_with([&args...] { return construct<T>(std::forward<Args>(args)...); }, target_cpu, nullptr)
            .and_then([](auto&& pair) {
                return pair.first.fix_pin()
                    .and_then([&pair]() { return sync_worker_object(pair.second, pair.first); })
                    .transform([&pair]() { return std::move(pair); });
            });
    }

    /**
     * @brief Executes a factory within an isolated CPU core scope.
     * @param factory Callable constructing the target object.
     * @param target_cpu Target core index, or auto-detect core.
     * @param worker_sync Optional kernel worker pool alignment callback.
     * @return Guard and constructed object pair, or isolation error.
     */
    template <IsolatedFactory F>
    [[nodiscard]] static auto make_isolated_with(F&& factory, std::int32_t target_cpu = config::kAffinityAutoCore,
        WorkerSyncCallback worker_sync = nullptr) noexcept
        -> std::expected<
            std::pair<CoreAffinityGuard, typename FactoryResultTraits<std::invoke_result_t<F>>::value_type>,
            IsolationError> {
        return isolate(target_cpu, std::move(worker_sync)).and_then([&factory](CoreAffinityGuard&& guard) {
            return execute_guarded_factory(std::move(guard), std::forward<F>(factory));
        });
    }

    /**
     * @brief Reverts thread affinity to pre-isolation state.
     */
    void restore() noexcept {
        if (state_ != IsolationState::Isolated || !original_mask_) { return; }

        if (posix::expect_success<posix::error_style::pthreads>(::pthread_setaffinity_np(
                ::pthread_self(), original_mask_.size_bytes(), original_mask_.get()))) [[likely]] {
            state_ = IsolationState::NotIsolated;
        }
    }

    /**
     * @brief Enforces thread affinity pinning integrity.
     * @return Success status or IsolationError context.
     */
    [[nodiscard]] std::expected<void, IsolationError> fix_pin() noexcept {
        if (state_ != IsolationState::Isolated || target_cpu_ < 0) {
            return std::unexpected(IsolationError {
                .ec      = posix::make_error(EINVAL),
                .context = "fix_pin (guard not pinned)",
            });
        }
        if (is_on_core()) [[likely]] { return {}; }

        /** @brief Bypass dynamic allocation for repeated drift correction within CPU_SETSIZE range. */
        if (toUInt(target_cpu_) < CPU_SETSIZE) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(target_cpu_, &mask);
            return pin_thread(&mask, sizeof(mask));
        }

        return apply_pinning(target_cpu_, num_cpus_).transform([](auto&&) noexcept {});
    }

    [[nodiscard]] IsolationState state() const noexcept { return state_; }
    [[nodiscard]] bool is_isolated() const noexcept { return state_ == IsolationState::Isolated; }
    [[nodiscard]] std::int32_t pinned_cpu() const noexcept { return target_cpu_; }
    [[nodiscard]] std::uint32_t num_cpus() const noexcept { return num_cpus_; }
    [[nodiscard]] explicit operator bool() const noexcept { return is_isolated(); }

private:
    constexpr CoreAffinityGuard(CpuSet orig_mask, std::int32_t target_cpu, std::uint32_t num_cpus) noexcept
        : original_mask_(std::move(orig_mask))
        , target_cpu_(target_cpu)
        , num_cpus_(num_cpus)
        , state_(IsolationState::Isolated) {}

    [[nodiscard]] static std::expected<CoreAffinityGuard, IsolationError> isolate(
        std::int32_t target_cpu = config::kAffinityAutoCore, WorkerSyncCallback worker_sync = nullptr) noexcept {
        const auto num_cpus_res { query_system_cpus() };
        if (!num_cpus_res) { return std::unexpected(num_cpus_res.error()); }
        const std::uint32_t num_cpus { *num_cpus_res };

        auto orig_res { capture_current_mask(num_cpus) };
        if (!orig_res) { return std::unexpected(orig_res.error()); }
        CpuSet original_mask { std::move(*orig_res) };

        const auto target_res { detect_optimal_cpu(target_cpu, num_cpus, original_mask) };
        if (!target_res) { return std::unexpected(target_res.error()); }
        std::int32_t detected_target { *target_res };

        auto working_res { apply_pinning(detected_target, num_cpus) };
        if (!working_res) { return std::unexpected(working_res.error()); }
        auto working_mask { std::move(*working_res) };

        CoreAffinityGuard guard { std::move(original_mask), detected_target, num_cpus };
        scope_exit cleanup_on_failure { [&guard] { guard.restore(); } };

        const auto reverify_res { reverify_and_repin(target_cpu, num_cpus, guard.target_cpu_, working_mask) };
        if (!reverify_res) { return std::unexpected(reverify_res.error()); }

        if (worker_sync) {
            const auto sync_res { worker_sync(
                working_mask.get(), working_mask.size_bytes(), num_cpus, guard.target_cpu_) };
            if (!sync_res) { return std::unexpected(sync_res.error()); }
        }

        cleanup_on_failure.release();
        return guard;
    }

    template <typename T, typename... Args>
        requires FactoryCreatable<T, Args...>
    [[nodiscard]] static auto construct(Args&&... args) {
        if constexpr (HasStaticCreate<T, Args...>) {
            return T::create(std::forward<Args>(args)...);
        } else {
            return T(std::forward<Args>(args)...);
        }
    }

    [[nodiscard]] bool is_on_core() const noexcept {
        if (state_ != IsolationState::Isolated || target_cpu_ < 0) { return false; }
        const auto current { posix::expect_result<posix::error_style::posix>(::sched_getcpu()) };
        return current && *current == target_cpu_;
    }

    template <ValidFactoryResult Res>
    [[nodiscard]] static auto to_expected(Res&& res) noexcept
        -> std::expected<typename FactoryResultTraits<std::remove_cvref_t<Res>>::value_type, IsolationError> {
        using Traits = FactoryResultTraits<std::remove_cvref_t<Res>>;
        if constexpr (Traits::is_expected) {
            return std::forward<Res>(res).transform_error([](const auto& err) noexcept { return map_error(err); });
        } else {
            return std::forward<Res>(res);
        }
    }

    template <IsolatedFactory Fn>
    [[nodiscard]] static auto execute_guarded_factory(CoreAffinityGuard guard, Fn&& factory_fn) noexcept
        -> std::expected<
            std::pair<CoreAffinityGuard, typename FactoryResultTraits<std::invoke_result_t<Fn>>::value_type>,
            IsolationError> {
        try {
            return to_expected(std::forward<Fn>(factory_fn)())
                .transform([guard = std::move(guard)](
                               auto&& obj) mutable { return std::pair { std::move(guard), std::move(obj) }; });
        } catch (const std::bad_alloc&) {
            return std::unexpected(IsolationError {
                .ec      = posix::make_error(ENOMEM),
                .context = "CoreAffinityGuard::make_isolated factory (bad_alloc)",
            });
        } catch (const std::exception&) {
            return std::unexpected(IsolationError {
                .ec      = posix::make_error(ECANCELED),
                .context = "CoreAffinityGuard::make_isolated factory (exception)",
            });
        } catch (...) {
            return std::unexpected(IsolationError {
                .ec      = posix::make_error(ENOTRECOVERABLE),
                .context = "CoreAffinityGuard::make_isolated factory (unknown)",
            });
        }
    }

    template <AnyErrorSource ErrT> [[nodiscard]] static constexpr std::error_code extract_ec(const ErrT& err) noexcept {
        using CleanErrT = std::remove_cvref_t<ErrT>;
        if constexpr (std::convertible_to<CleanErrT, std::error_code>) {
            return err;
        } else if constexpr (requires { err.ec; }) {
            return extract_ec(err.ec);
        } else if constexpr (is_variant_v<CleanErrT>) {
            return std::visit([](const auto& v) noexcept { return extract_ec(v); }, err);
        } else if constexpr (std::is_enum_v<CleanErrT>) {
            return posix::make_error(std::to_underlying(err));
        } else if constexpr (std::is_integral_v<CleanErrT>) {
            return posix::make_error(err);
        } else {
            return posix::make_error(EIO);
        }
    }

    template <HasSetWorkerAffinity T>
    [[nodiscard]] static auto invoke_affinity_api(T& obj, const CpuSet& mask, const CoreAffinityGuard& guard) noexcept {
        return obj.set_worker_affinity(mask.get(), mask.size_bytes(), guard.num_cpus(), guard.pinned_cpu())
            .transform([](auto&&...) noexcept {})
            .transform_error([](const auto& err) noexcept {
                return IsolationError { .ec = extract_ec(err), .context = "set_worker_affinity failed" };
            });
    }

    template <HasRegisterWorkerAffinity T>
        requires(!HasSetWorkerAffinity<T>)
    [[nodiscard]] static auto invoke_affinity_api(T& obj, const CpuSet& mask, const CoreAffinityGuard&) noexcept {
        return obj.register_worker_affinity(mask.get(), mask.size_bytes())
            .transform([](auto&&...) noexcept {})
            .transform_error([](const auto& err) noexcept {
                return IsolationError { .ec = extract_ec(err), .context = "register_worker_affinity failed" };
            });
    }

    template <WorkerSyncObject T>
    [[nodiscard]] static auto apply_worker_affinity(T& obj, const CoreAffinityGuard& guard) noexcept {
        return CpuSet::allocate(guard.num_cpus())
            .transform_error(
                [](const auto& ec) noexcept { return IsolationError { .ec = ec, .context = "CPU_ALLOC" }; })
            .and_then([&obj, &guard](CpuSet&& mask) noexcept {
                mask.set(toUInt(guard.pinned_cpu()));
                return invoke_affinity_api(obj, mask, guard);
            });
    }

    template <typename T>
        requires std::is_object_v<T>
    [[nodiscard]] static std::expected<void, IsolationError> sync_worker_object(
        T& obj, const CoreAffinityGuard& guard) noexcept {
        if constexpr (WorkerSyncObject<T>) {
            return apply_worker_affinity(obj, guard);
        } else {
            return {};
        }
    }

    template <AnyErrorSource ErrT> [[nodiscard]] static constexpr IsolationError map_error(const ErrT& err) noexcept {
        if constexpr (std::convertible_to<ErrT, IsolationError>) {
            return err;
        } else {
            return IsolationError {
                .ec      = extract_ec(err),
                .context = "CoreAffinityGuard::make_isolated factory",
            };
        }
    }

    [[nodiscard]] static std::expected<std::uint32_t, IsolationError> query_system_cpus() noexcept {
        const auto cpus { posix::expect_result<posix::error_style::posix>(::sysconf(_SC_NPROCESSORS_ONLN)) };
        if (!cpus) { return std::unexpected(IsolationError { .ec = cpus.error(), .context = "sysconf" }); }
        if (*cpus < 1) {
            return std::unexpected(IsolationError { .ec = posix::make_error(ENOTSUP), .context = "sysconf" });
        }
        return toUInt(*cpus);
    }

    [[nodiscard]] static std::expected<CpuSet, IsolationError> capture_current_mask(std::uint32_t max_cpus) noexcept {
        auto mask_res { CpuSet::allocate(max_cpus) };
        if (!mask_res) { return std::unexpected(IsolationError { .ec = mask_res.error(), .context = "CPU_ALLOC" }); }
        const auto get_res { posix::expect_success<posix::error_style::pthreads>(
            ::pthread_getaffinity_np(::pthread_self(), mask_res->size_bytes(), mask_res->get())) };
        if (!get_res) {
            return std::unexpected(IsolationError { .ec = get_res.error(), .context = "pthread_getaffinity_np" });
        }
        return std::move(*mask_res);
    }

    [[nodiscard]] static std::expected<void, IsolationError> pin_thread(
        const cpu_set_t* mask, std::size_t mask_size) noexcept {
        return posix::expect_success<posix::error_style::pthreads>(
            ::pthread_setaffinity_np(::pthread_self(), mask_size, mask))
            .transform_error([](const std::error_code& ec) noexcept {
                return IsolationError { .ec = ec, .context = "pthread_setaffinity_np" };
            });
    }

    [[nodiscard]] static std::expected<CpuSet, IsolationError> apply_pinning(
        std::int32_t target_cpu, std::uint32_t num_cpus) noexcept {
        auto mask_res { CpuSet::allocate(num_cpus) };
        if (!mask_res) { return std::unexpected(IsolationError { .ec = mask_res.error(), .context = "CPU_ALLOC" }); }
        mask_res->set(toUInt(target_cpu));
        return pin_thread(mask_res->get(), mask_res->size_bytes()).transform([&mask_res]() noexcept {
            return std::move(*mask_res);
        });
    }

    [[nodiscard]] static std::expected<std::int32_t, IsolationError> detect_optimal_cpu(
        std::int32_t requested_cpu, std::uint32_t num_cpus, const CpuSet& orig_mask) noexcept {
        if (requested_cpu >= 0 && toUInt(requested_cpu) < num_cpus) { return requested_cpu; }

        if (requested_cpu >= 0) {
            return std::unexpected(IsolationError {
                .ec      = posix::make_error(EINVAL),
                .context = "detect_optimal_cpu (requested CPU out of bounds)",
            });
        }

        const auto current { posix::expect_result<posix::error_style::posix>(::sched_getcpu()) };
        if (current && *current >= 0 && toUInt(*current) < num_cpus && orig_mask.is_set(toUInt(*current))) {
            return *current;
        }

        if (const auto first_cpu { orig_mask.first_set_cpu() }; first_cpu && *first_cpu < num_cpus) {
            return toInt(*first_cpu);
        }

        return std::unexpected(IsolationError {
            .ec      = posix::make_error(ENODATA),
            .context = "detect_optimal_cpu (no available CPU detected in affinity mask)",
        });
    }

    [[nodiscard]] static std::expected<void, IsolationError> reverify_and_repin(
        std::int32_t requested_cpu, std::uint32_t num_cpus, std::int32_t& target_cpu, CpuSet& working_mask) noexcept {
        if (requested_cpu >= 0) { return {}; }
        const auto verify_cpu { posix::expect_result<posix::error_style::posix>(::sched_getcpu()) };
        if (!verify_cpu || *verify_cpu < 0 || toUInt(*verify_cpu) >= num_cpus) {
            return std::unexpected(IsolationError {
                .ec      = verify_cpu ? posix::make_error(ERANGE) : verify_cpu.error(),
                .context = "reverify_and_repin (sched_getcpu verification failed)",
            });
        }
        if (*verify_cpu == target_cpu) { return {}; }
        auto repin_res { apply_pinning(*verify_cpu, num_cpus) };
        if (!repin_res) { return std::unexpected(repin_res.error()); }
        target_cpu   = *verify_cpu;
        working_mask = std::move(*repin_res);
        return {};
    }

    CpuSet original_mask_ {};
    std::int32_t target_cpu_ { -1 };
    std::uint32_t num_cpus_ { 0u };
    IsolationState state_ { IsolationState::NotIsolated };
};

} // namespace affinity

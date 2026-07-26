/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "config.hpp"
#include "numeric_cast.hpp"
#include "posix.hpp"
#include "posix_error.hpp"

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

/**
 * @file affinity.hpp
 * @brief RAII CPU core isolation and worker thread affinity management.
 *
 * Provides single-core thread pinning with deterministic restoration on scope exit,
 * alongside factory helpers for isolated object construction.
 */
namespace affinity {

/**
 * @brief Strong type tag explicitly targeting a specific CPU core for isolation.
 */
struct TargetCpu {
    std::int32_t core { config::kAffinityAutoCore };
    constexpr explicit TargetCpu(std::int32_t c) noexcept
        : core(c) {}
};

/**
 * @brief Structured error context for CPU isolation failures.
 */
struct IsolationError {
    std::error_code ec {}; /**< POSIX or system error code. */
    std::string_view context {}; /**< Failure point description. */
};

/**
 * @brief Logical state of thread isolation and worker synchronization.
 */
enum class IsolationState : std::uint8_t {
    NotIsolated, /**< Thread affinity is unpinned or restored to initial state. */
    PinnedOnly, /**< Thread is pinned to target CPU without worker pool sync. */
    PinnedAndSynced, /**< Thread is pinned and worker pool synchronization succeeded. */
};

/**
 * @brief Dynamic RAII wrapper for POSIX cpu_set_t masks.
 */
class CpuSet {
public:
    constexpr CpuSet() noexcept = default;

    /**
     * @brief Allocates a CPU mask sized for the specified processor count.
     * @param num_cpus Total processors to allocate space for.
     * @return Allocated CpuSet instance or OS error code on failure.
     */
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

    ~CpuSet() noexcept = default;

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
        const std::uint32_t total_cpus { num_cpus_ > 0u ? num_cpus_ : toUInt(capacity_bits()) };
        const auto cpus_range { std::views::iota(0u, total_cpus) };
        const auto match { std::ranges::find_if(
            cpus_range, [this](std::uint32_t cpu) noexcept { return is_set(cpu); }) };
        if (match != cpus_range.end()) { return *match; }
        return std::nullopt;
    }

    [[nodiscard]] cpu_set_t* get() noexcept { return ptr_.get(); }
    [[nodiscard]] const cpu_set_t* get() const noexcept { return ptr_.get(); }

    [[nodiscard]] std::size_t size_bytes() const noexcept { return mask_size_; }
    [[nodiscard]] std::size_t capacity_bits() const noexcept { return mask_size_ * 8uz; }
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
    const cpu_set_t*, std::size_t, std::uint32_t, std::int32_t) const noexcept>;

/**
 * @brief Concept constraining callables compatible with WorkerSyncCallback.
 */
template <typename F>
concept WorkerSyncCallable
    = requires(F&& f, const cpu_set_t* mask, std::size_t size, std::uint32_t cpus, std::int32_t target_cpu) {
          {
              std::forward<F>(f)(mask, size, cpus, target_cpu)
          } -> std::convertible_to<std::expected<void, IsolationError>>;
      };

/**
 * @brief Concept constraining factory callables compatible with make_guarded.
 */
template <typename F>
concept IsolatedFactory = std::invocable<F> && requires {
    typename std::invoke_result_t<F>::value_type;
    typename std::invoke_result_t<F>::error_type;
} && (std::convertible_to<typename std::invoke_result_t<F>::error_type, IsolationError> || std::convertible_to<typename std::invoke_result_t<F>::error_type, std::error_code>);

/**
 * @brief Concept constraining types supporting set_worker_affinity.
 */
template <typename T>
concept HasSetWorkerAffinity
    = requires(T& obj, const cpu_set_t* mask, std::size_t size, std::uint32_t cpus, std::int32_t target_cpu) {
          { obj.set_worker_affinity(mask, size, cpus, target_cpu) };
      };

/**
 * @brief Concept constraining types supporting register_worker_affinity.
 */
template <typename T>
concept HasRegisterWorkerAffinity = requires(T& obj, const cpu_set_t* mask, std::size_t size) {
    { obj.register_worker_affinity(mask, size) };
};

/**
 * @brief Concept constraining objects supporting direct worker affinity injection at compile time.
 */
template <typename T>
concept WorkerSyncObject = HasSetWorkerAffinity<T> || HasRegisterWorkerAffinity<T>;

/**
 * @brief Concept constraining objects that do not support worker affinity injection.
 */
template <typename T>
concept NonWorkerSyncObject = !WorkerSyncObject<T>;

/**
 * @brief Concept constraining types supportable by make_isolated<T> factory construction.
 */
template <typename T, typename... Args>
concept FactoryCreatable = requires(Args&&... args) {
    { T::create(std::forward<Args>(args)...) } -> std::convertible_to<std::expected<T, std::error_code>>;
};

/**
 * @brief RAII controller pinning the executing thread to a specific CPU core.
 *
 * Captures initial thread affinity upon construction and restores it on destruction.
 *
 * @note Thread affinity operations apply exclusively to the calling thread.
 * Construction and destruction MUST occur on the exact same thread.
 */
class [[nodiscard]] CoreAffinityGuard {
public:
    /**
     * @brief Pins the calling thread to a target CPU core or active core.
     * @param target_cpu Target core index, or kAffinityAutoCore to detect active core.
     */
    explicit CoreAffinityGuard(std::int32_t target_cpu = config::kAffinityAutoCore) noexcept {
        isolate_impl(target_cpu, nullptr);
    }

    /**
     * @brief Pins the calling thread and invokes worker pool synchronization.
     * @param target_cpu Target core index.
     * @param worker_sync Callback for kernel worker pool alignment.
     */
    template <WorkerSyncCallable F> explicit CoreAffinityGuard(std::int32_t target_cpu, F&& worker_sync) noexcept {
        isolate_impl(target_cpu, WorkerSyncCallback { std::forward<F>(worker_sync) });
    }

    /**
     * @brief Pins the calling thread to active core and invokes worker pool synchronization.
     * @param worker_sync Callback for kernel worker pool alignment.
     */
    template <WorkerSyncCallable F> explicit CoreAffinityGuard(F&& worker_sync) noexcept {
        isolate_impl(config::kAffinityAutoCore, WorkerSyncCallback { std::forward<F>(worker_sync) });
    }

    /**
     * @brief Restores the original thread affinity mask captured at construction.
     */
    ~CoreAffinityGuard() noexcept { restore(); }

    CoreAffinityGuard(const CoreAffinityGuard&)            = delete;
    CoreAffinityGuard& operator=(const CoreAffinityGuard&) = delete;

    CoreAffinityGuard(CoreAffinityGuard&& other) noexcept
        : original_mask_(std::move(other.original_mask_))
        , error_(other.error_)
        , target_cpu_(std::exchange(other.target_cpu_, -1))
        , num_cpus_(std::exchange(other.num_cpus_, 0u))
        , state_(std::exchange(other.state_, IsolationState::NotIsolated))
        , is_pinned_(std::exchange(other.is_pinned_, false)) {}

    CoreAffinityGuard& operator=(CoreAffinityGuard&& other) noexcept {
        if (this != &other) [[likely]] {
            restore();
            original_mask_ = std::move(other.original_mask_);
            error_         = other.error_;
            target_cpu_    = std::exchange(other.target_cpu_, -1);
            num_cpus_      = std::exchange(other.num_cpus_, 0u);
            state_         = std::exchange(other.state_, IsolationState::NotIsolated);
            is_pinned_     = std::exchange(other.is_pinned_, false);
        }
        return *this;
    }

    /**
     * @brief Restores the original thread affinity mask captured at construction.
     */
    void restore() noexcept {
        if (is_pinned_ && original_mask_) [[likely]] {
            ::pthread_setaffinity_np(::pthread_self(), original_mask_.size_bytes(), original_mask_.get());
            is_pinned_ = false;
            state_     = IsolationState::NotIsolated;
        }
    }

    [[nodiscard]] IsolationState state() const noexcept { return state_; }
    [[nodiscard]] bool is_isolated() const noexcept { return state_ != IsolationState::NotIsolated; }
    [[nodiscard]] std::int32_t pinned_cpu() const noexcept { return target_cpu_; }
    [[nodiscard]] std::uint32_t num_cpus() const noexcept { return num_cpus_; }
    [[nodiscard]] const IsolationError& error() const noexcept { return error_; }
    [[nodiscard]] explicit operator bool() const noexcept { return is_isolated(); }

    /**
     * @brief Checks in real-time whether the calling thread is currently executing on the pinned CPU core.
     * @return True if active CPU core matches the target pinned core index.
     */
    [[nodiscard]] bool is_on_core() const noexcept {
        if (!is_pinned_ || target_cpu_ < 0) { return false; }
        const auto current { posix::expect_result<posix::error_style::posix>(::sched_getcpu()) };
        return current && *current == target_cpu_;
    }

    /**
     * @brief Proactively verifies and re-applies thread affinity if execution core has drifted.
     * @return Success status or IsolationError context.
     */
    [[nodiscard]] std::expected<void, IsolationError> fix_pin() noexcept {
        if (!is_pinned_ || target_cpu_ < 0) {
            return std::unexpected(IsolationError {
                .ec      = posix::make_error(EINVAL),
                .context = "fix_pin (guard not pinned)",
            });
        }
        if (is_on_core()) [[likely]] { return {}; }

        auto pin_res { apply_pinning(target_cpu_, num_cpus_) };
        if (pin_res) [[likely]] { return {}; }
        error_ = pin_res.error();
        return std::unexpected(error_);
    }

    /**
     * @brief Executes a factory within an isolated CPU core scope, returning the guard and object.
     *
     * @param target_cpu Target core index for pinning.
     * @param worker_sync Callback for kernel worker pool alignment.
     * @param factory Callable constructing the object returning std::expected<T, std::error_code>.
     */
    template <typename F>
        requires IsolatedFactory<F>
    [[nodiscard]] static auto make_isolated(
        std::int32_t target_cpu, WorkerSyncCallback worker_sync, F&& factory) noexcept
        -> std::expected<std::pair<CoreAffinityGuard, typename std::invoke_result_t<F>::value_type>, IsolationError> {
        CoreAffinityGuard guard { worker_sync ? CoreAffinityGuard { target_cpu, std::move(worker_sync) }
                                               : CoreAffinityGuard { target_cpu } };
        return execute_guarded_factory(std::move(guard), std::forward<F>(factory));
    }

    /** @overload Targets the calling thread's current core, no worker sync. */
    template <typename F>
        requires IsolatedFactory<F>
    [[nodiscard]] static auto make_isolated(F&& factory) noexcept
        -> std::expected<std::pair<CoreAffinityGuard, typename std::invoke_result_t<F>::value_type>, IsolationError> {
        return make_isolated(config::kAffinityAutoCore, nullptr, std::forward<F>(factory));
    }

    /**
     * @brief Direct type-deducted factory isolating specified target_cpu before constructing T in-place.
     * Automatically registers worker affinity if T supports set_worker_affinity or register_worker_affinity.
     */
    template <typename T, typename... Args>
        requires FactoryCreatable<T, Args...>
    [[nodiscard]] static auto make_isolated(TargetCpu target_cpu, Args&&... args) noexcept
        -> std::expected<std::pair<CoreAffinityGuard, T>, IsolationError> {
        return make_isolated(target_cpu.core, nullptr, [&args...]() {
            return T::create(std::forward<Args>(args)...);
        }).and_then([](std::pair<CoreAffinityGuard, T>&& pair) {
            return pair.first.fix_pin()
                .and_then([&pair]() { return sync_worker_object(pair.second, pair.first); })
                .transform([pair = std::move(pair)]() mutable { return std::move(pair); });
        });
    }

    /**
     * @brief Direct type-deducted factory isolating calling thread's current core before constructing T.
     */
    template <typename T, typename... Args>
        requires FactoryCreatable<T, Args...>
    [[nodiscard]] static auto make_isolated(Args&&... args) noexcept
        -> std::expected<std::pair<CoreAffinityGuard, T>, IsolationError> {
        return make_isolated<T>(TargetCpu { config::kAffinityAutoCore }, std::forward<Args>(args)...);
    }

    /**
     * @brief Explicitly-named factory method pinning to a specific CPU core.
     */
    template <typename T, typename... Args>
        requires FactoryCreatable<T, Args...>
    [[nodiscard]] static auto make_isolated_at(std::int32_t target_cpu, Args&&... args) noexcept
        -> std::expected<std::pair<CoreAffinityGuard, T>, IsolationError> {
        return make_isolated<T>(TargetCpu { target_cpu }, std::forward<Args>(args)...);
    }

private:
    template <IsolatedFactory Fn>
    [[nodiscard]] static auto execute_guarded_factory(CoreAffinityGuard guard, Fn&& factory_fn) noexcept
        -> std::expected<std::pair<CoreAffinityGuard, typename std::invoke_result_t<Fn>::value_type>, IsolationError> {
        using ResultT = typename std::invoke_result_t<Fn>::value_type;
        if (!guard) { return std::unexpected(guard.error()); }

        try {
            auto obj_res { std::forward<Fn>(factory_fn)() };
            if (!obj_res) { return std::unexpected(map_error(obj_res.error())); }
            return std::pair<CoreAffinityGuard, ResultT> { std::move(guard), std::move(*obj_res) };
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

    template <typename ErrT> [[nodiscard]] static constexpr std::error_code extract_ec(const ErrT& err) noexcept {
        if constexpr (std::convertible_to<ErrT, std::error_code>) {
            return err;
        } else if constexpr (requires { err.ec; }) {
            return err.ec;
        } else {
            return posix::make_error(EIO);
        }
    }

    template <HasSetWorkerAffinity T>
    [[nodiscard]] static std::expected<void, IsolationError> sync_object(
        T& obj, const cpu_set_t* mask, std::size_t size, std::uint32_t cpus, std::int32_t target) noexcept {
        return obj.set_worker_affinity(mask, size, cpus, target)
            .transform([](auto&&...) noexcept {})
            .transform_error([](const auto& err) noexcept {
                return IsolationError {
                    .ec      = extract_ec(err),
                    .context = "set_worker_affinity failed",
                };
            });
    }

    template <HasRegisterWorkerAffinity T>
        requires(!HasSetWorkerAffinity<T>)
    [[nodiscard]] static std::expected<void, IsolationError> sync_object(
        T& obj, const cpu_set_t* mask, std::size_t size, std::uint32_t, std::int32_t) noexcept {
        return obj.register_worker_affinity(mask, size)
            .transform([](auto&&...) noexcept {})
            .transform_error([](const auto& err) noexcept {
                return IsolationError {
                    .ec      = extract_ec(err),
                    .context = "register_worker_affinity failed",
                };
            });
    }

    template <WorkerSyncObject T>
    [[nodiscard]] static std::expected<void, IsolationError> sync_worker_object(
        T& obj, const CoreAffinityGuard& guard) noexcept {
        auto mask_res { CpuSet::allocate(guard.num_cpus()) };
        if (!mask_res) { return std::unexpected(IsolationError { .ec = mask_res.error(), .context = "CPU_ALLOC" }); }
        auto mask { std::move(*mask_res) };
        mask.set(toUInt(guard.pinned_cpu()));
        return sync_object(obj, mask.get(), mask.size_bytes(), guard.num_cpus(), guard.pinned_cpu());
    }

    template <NonWorkerSyncObject T>
    [[nodiscard]] static std::expected<void, IsolationError> sync_worker_object(T&, const CoreAffinityGuard&) noexcept {
        return {};
    }

    template <typename ErrT> [[nodiscard]] static constexpr IsolationError map_error(const ErrT& err) noexcept {
        if constexpr (std::convertible_to<ErrT, IsolationError>) {
            return err;
        } else if constexpr (std::convertible_to<ErrT, std::error_code>) {
            return IsolationError {
                .ec      = err,
                .context = "CoreAffinityGuard::make_isolated factory",
            };
        } else {
            return IsolationError {
                .ec      = posix::make_error(err),
                .context = "CoreAffinityGuard::make_isolated factory",
            };
        }
    }

    /**
     * @brief Queries the number of configured online CPU cores from the OS.
     */
    [[nodiscard]] static std::expected<std::uint32_t, IsolationError> query_system_cpus() noexcept {
        const auto cpus_res { posix::expect_result<posix::error_style::posix>(::sysconf(_SC_NPROCESSORS_ONLN)) };
        if (!cpus_res || *cpus_res < 1) {
            return std::unexpected(IsolationError {
                .ec      = cpus_res ? posix::make_error(ENOTSUP) : cpus_res.error(),
                .context = "sysconf",
            });
        }
        return toUInt(*cpus_res);
    }

    /**
     * @brief Allocates and captures the calling thread's current affinity mask.
     */
    [[nodiscard]] static std::expected<CpuSet, IsolationError> capture_current_mask(std::uint32_t max_cpus) noexcept {
        auto orig_res { CpuSet::allocate(max_cpus) };
        if (!orig_res) { return std::unexpected(IsolationError { .ec = orig_res.error(), .context = "CPU_ALLOC" }); }
        auto mask { std::move(*orig_res) };
        const auto get_res { posix::expect_success<posix::error_style::pthreads>(
            ::pthread_getaffinity_np(::pthread_self(), mask.size_bytes(), mask.get())) };
        if (!get_res) {
            return std::unexpected(IsolationError { .ec = get_res.error(), .context = "pthread_getaffinity_np" });
        }
        return mask;
    }

    /**
     * @brief Allocates working mask and pins the calling thread to target CPU core.
     */
    [[nodiscard]] static std::expected<CpuSet, IsolationError> apply_pinning(
        std::int32_t target_cpu, std::uint32_t num_cpus) noexcept {
        const std::uint32_t max_cpus { std::max(num_cpus, toUInt(target_cpu) + 1u) };
        auto working_res { CpuSet::allocate(max_cpus) };
        if (!working_res) {
            return std::unexpected(IsolationError { .ec = working_res.error(), .context = "CPU_ALLOC" });
        }
        auto working_mask { std::move(*working_res) };
        working_mask.zero();
        working_mask.set(toUInt(target_cpu));

        const auto pin_res { posix::expect_success<posix::error_style::pthreads>(
            ::pthread_setaffinity_np(::pthread_self(), working_mask.size_bytes(), working_mask.get())) };
        if (!pin_res) {
            return std::unexpected(IsolationError { .ec = pin_res.error(), .context = "pthread_setaffinity_np" });
        }
        return working_mask;
    }

    /**
     * @brief Smartly detects the optimal CPU core for isolation dynamically from OS topology.
     *
     * If requested_cpu is non-negative, validates it against online CPUs.
     * If requested_cpu < 0 (auto mode), queries the executing thread's active core via sched_getcpu,
     * falling back to the first available core in the process affinity mask.
     */
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
        if (current && *current >= 0 && toUInt(*current) < num_cpus) { return *current; }

        if (const auto first_cpu { orig_mask.first_set_cpu() }; first_cpu && *first_cpu < num_cpus) {
            return toInt(*first_cpu);
        }

        return std::unexpected(IsolationError {
            .ec      = posix::make_error(ENODATA),
            .context = "detect_optimal_cpu (no available CPU detected in affinity mask)",
        });
    }

    /**
     * @brief Re-verifies active core and re-applies pinning in-place if thread migrated during setup window.
     */
    [[nodiscard]] static std::expected<void, IsolationError> reverify_and_repin(
        std::int32_t requested_cpu, std::uint32_t num_cpus, std::int32_t& target_cpu, CpuSet& working_mask) noexcept {
        if (requested_cpu >= 0) { return {}; }
        const auto verify_cpu { posix::expect_result<posix::error_style::posix>(::sched_getcpu()) };
        if (!verify_cpu || *verify_cpu < 0 || toUInt(*verify_cpu) >= num_cpus || *verify_cpu == target_cpu) {
            return {};
        }
        auto repin_res { apply_pinning(*verify_cpu, num_cpus) };
        if (!repin_res) { return std::unexpected(repin_res.error()); }
        target_cpu   = *verify_cpu;
        working_mask = std::move(*repin_res);
        return {};
    }

    /**
     * @brief Internal isolation pipeline orchestrating affinity capturing, pinning, and synchronization.
     */
    void isolate_impl(std::int32_t requested_cpu, WorkerSyncCallback worker_sync) noexcept {
        const auto num_cpus_res { query_system_cpus() };
        if (!num_cpus_res) {
            error_ = num_cpus_res.error();
            return;
        }
        const std::uint32_t num_cpus { *num_cpus_res };

        const std::uint32_t query_cpus { std::max(num_cpus, requested_cpu >= 0 ? toUInt(requested_cpu) + 1u : 1u) };
        auto orig_res { capture_current_mask(query_cpus) };
        if (!orig_res) {
            error_ = orig_res.error();
            return;
        }
        original_mask_ = std::move(*orig_res);

        const auto target_res { detect_optimal_cpu(requested_cpu, num_cpus, original_mask_) };
        if (!target_res) {
            error_ = target_res.error();
            return;
        }
        std::int32_t target_cpu { *target_res };

        auto working_res { apply_pinning(target_cpu, num_cpus) };
        if (!working_res) {
            error_ = working_res.error();
            return;
        }
        auto working_mask { std::move(*working_res) };

        is_pinned_ = true;
        num_cpus_  = num_cpus;

        const auto reverify_res { reverify_and_repin(requested_cpu, num_cpus, target_cpu, working_mask) };
        target_cpu_ = target_cpu;
        if (!reverify_res) {
            error_ = reverify_res.error();
            restore();
            state_ = IsolationState::NotIsolated;
            return;
        }

        if (!worker_sync) {
            state_ = IsolationState::PinnedOnly;
            return;
        }

        const auto sync_res { worker_sync(working_mask.get(), working_mask.size_bytes(), num_cpus, target_cpu) };
        if (!sync_res) {
            error_ = sync_res.error();
            restore();
            state_ = IsolationState::NotIsolated;
            return;
        }

        state_ = IsolationState::PinnedAndSynced;
    }

    CpuSet original_mask_ {};
    IsolationError error_ {};
    std::int32_t target_cpu_ { -1 };
    std::uint32_t num_cpus_ { 0u };
    IsolationState state_ { IsolationState::NotIsolated };
    bool is_pinned_ { false };
};

} // namespace affinity

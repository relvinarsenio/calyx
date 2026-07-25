/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "numeric_cast.hpp"
#include "posix_error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <string_view>
#include <system_error>
#include <utility>

namespace affinity {

/**
 * @brief RAII wrapper bundling dynamic cpu_set_t allocation with mask size and bounds checking.
 */
class CpuSet {
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

public:
    constexpr CpuSet() noexcept = default;

    /**
     * @brief Factory method allocating a CpuSet for the requested number of CPUs.
     * @return The allocated CpuSet, or the real OS error code on failure.
     */
    [[nodiscard]] static std::expected<CpuSet, std::error_code> allocate(std::uint32_t num_cpus) noexcept {
        if (num_cpus == 0u) [[unlikely]] { return std::unexpected(posix::make_error(EINVAL)); }

        const std::size_t mask_size = CPU_ALLOC_SIZE(num_cpus);
        const auto raw              = posix::expect_result<posix::error_style::pointer>(CPU_ALLOC(num_cpus));
        if (!raw) [[unlikely]] { return std::unexpected(raw.error()); }

        CpuSet set(*raw, mask_size, num_cpus);
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

    bool clear(std::uint32_t cpu) noexcept {
        if (ptr_ && cpu < capacity_bits()) [[likely]] {
            CPU_CLR_S(cpu, mask_size_, ptr_.get());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool is_set(std::uint32_t cpu) const noexcept {
        if (ptr_ && cpu < capacity_bits()) [[likely]] { return CPU_ISSET_S(cpu, mask_size_, ptr_.get()) != 0; }
        return false;
    }

    [[nodiscard]] cpu_set_t* get() noexcept { return ptr_.get(); }
    [[nodiscard]] const cpu_set_t* get() const noexcept { return ptr_.get(); }

    [[nodiscard]] std::size_t size_bytes() const noexcept { return mask_size_; }
    [[nodiscard]] std::uint32_t num_cpus() const noexcept { return num_cpus_; }
    [[nodiscard]] std::size_t capacity_bits() const noexcept { return mask_size_ * 8uz; }
    [[nodiscard]] bool empty() const noexcept { return ptr_ == nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

/**
 * @brief Structured error type for affinity operations.
 */
struct IsolationError {
    std::error_code ec {};

    /**
     * @brief Failure context.
     * @note Must be a static string literal to avoid dangling references.
     */
    std::string_view context {};
};

/**
 * @brief Result of an affinity synchronization operation.
 */
struct SyncResult {
    bool main_thread_ok     = false;
    bool kernel_worker_ok   = false;
    std::int32_t target_cpu = -1;
    std::uint32_t num_cpus  = 0u;
};

/**
 * @brief Callback for aligning kernel worker pools with thread affinity.
 */
using WorkerSyncCallback = std::move_only_function<std::expected<void, IsolationError>(
    const cpu_set_t*, std::size_t, std::uint32_t, std::int32_t) const noexcept>;

namespace isolation_impl {

struct PinAttempt {
    std::error_code ec {};
    std::int32_t target_cpu { -1 };
};

[[nodiscard]] inline auto try_pin(CpuSet& mask, std::int32_t target_cpu) noexcept -> PinAttempt {
    /**
     * @brief Prevent out-of-bounds writes on sparse CPU topologies where active CPU IDs exceed mask capacity.
     */
    if (target_cpu < 0 || toUInt(target_cpu) >= mask.capacity_bits()) {
        return PinAttempt { .ec = std::make_error_code(std::errc::invalid_argument), .target_cpu = target_cpu };
    }
    mask.zero();
    mask.set(toUInt(target_cpu));

    const auto res = posix::expect_success<posix::error_style::pthreads>(
        ::pthread_setaffinity_np(::pthread_self(), mask.size_bytes(), mask.get()));
    return PinAttempt { .ec = res ? std::error_code {} : res.error(), .target_cpu = target_cpu };
}

[[nodiscard]] inline auto sync_workers(const WorkerSyncCallback& worker_sync, const CpuSet& mask,
    std::uint32_t num_configured_cpus, std::int32_t target_cpu) noexcept -> std::expected<bool, IsolationError> {
    if (!worker_sync) { return false; }
    return worker_sync(mask.get(), mask.size_bytes(), num_configured_cpus, target_cpu).transform([]() { return true; });
}

[[nodiscard]] inline auto sync_isolation(CpuSet& mask, std::uint32_t num_configured_cpus,
    const std::int32_t initial_cpu, const WorkerSyncCallback& worker_sync) noexcept
    -> std::expected<SyncResult, IsolationError> {
    const auto first_attempt = try_pin(mask, initial_cpu);
    if (first_attempt.ec) {
        return std::unexpected(IsolationError { .ec = first_attempt.ec, .context = "pthread_setaffinity_np" });
    }

    /**
     * @brief Mitigate scheduling race conditions (TOCTOU) where the OS
     * migrates the thread immediately after or during the initial pin.
     */
    const auto final_cpu = posix::expect_result<posix::error_style::posix>(::sched_getcpu());
    if (!final_cpu) { return std::unexpected(IsolationError { .ec = final_cpu.error(), .context = "sched_getcpu" }); }

    const auto attempt = (*final_cpu != first_attempt.target_cpu) ? try_pin(mask, *final_cpu) : first_attempt;
    if (attempt.ec) {
        return std::unexpected(IsolationError { .ec = attempt.ec, .context = "pthread_setaffinity_np" });
    }

    const auto worker_res = sync_workers(worker_sync, mask, num_configured_cpus, attempt.target_cpu);
    if (!worker_res) { return std::unexpected(worker_res.error()); }

    return SyncResult { .main_thread_ok = true,
        .kernel_worker_ok               = *worker_res,
        .target_cpu                     = attempt.target_cpu,
        .num_cpus                       = num_configured_cpus };
}

} // namespace isolation_impl

/**
 * @brief Restrict execution of the calling thread to a single stabilized core.
 *
 * Stabilizes affinity by performing verified pinning and propagating the resulting
 * mask to asynchronous kernel worker pools.
 *
 * @param worker_sync Callback to align kernel workers with the stabilized core.
 * @return Results of the isolation and worker synchronization attempt.
 */
[[nodiscard]] inline auto execute_strict_isolation(WorkerSyncCallback worker_sync = nullptr) noexcept
    -> std::expected<SyncResult, IsolationError> {
    const auto processors_available = posix::expect_result<posix::error_style::posix>(::sysconf(_SC_NPROCESSORS_CONF));
    if (!processors_available) {
        return std::unexpected(IsolationError { .ec = processors_available.error(), .context = "sysconf" });
    }
    if (*processors_available < 1) {
        return std::unexpected(IsolationError { .ec = posix::make_error(ENOTSUP), .context = "sysconf" });
    }

    const auto current_cpu = posix::expect_result<posix::error_style::posix>(::sched_getcpu());
    if (!current_cpu) {
        return std::unexpected(IsolationError { .ec = current_cpu.error(), .context = "sched_getcpu" });
    }

    const std::uint32_t num_configured_cpus = toUInt(*processors_available);
    const std::uint32_t current_cpu_val     = toUInt(*current_cpu);
    const std::uint32_t max_cpu_needed      = std::max(num_configured_cpus, current_cpu_val + 1u);

    auto mask_res = CpuSet::allocate(max_cpu_needed);
    if (!mask_res) [[unlikely]] {
        return std::unexpected(IsolationError { .ec = mask_res.error(), .context = "CPU_ALLOC" });
    }

    return isolation_impl::sync_isolation(*mask_res, num_configured_cpus, *current_cpu, worker_sync);
}

} // namespace affinity

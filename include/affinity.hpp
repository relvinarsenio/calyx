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
#include "utils.hpp"

#include <cerrno>
#include <expected>
#include <functional>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <string_view>
#include <system_error>

namespace affinity {

using cpu_set_ptr = std::unique_ptr<cpu_set_t, decltype([](cpu_set_t* p) noexcept { CPU_FREE(p); })>;

/**
 * @brief Structured error type for affinity operations.
 */
struct IsolationError {
    std::error_code ec;

    /**
     * @brief Failure context.
     * @note Must be a static string literal to avoid dangling references.
     */
    std::string_view context;
};

/**
 * @brief Result of an affinity synchronization operation.
 */
struct SyncResult {
    bool main_thread_ok     = false;
    bool kernel_worker_ok   = false;
    std::int32_t target_cpu = -1;
    std::uint32_t num_cpus  = 0;
};

/**
 * @brief Callback for aligning kernel worker pools with thread affinity.
 */
using WorkerSyncCallback = std::move_only_function<std::expected<void, IsolationError>(
    const cpu_set_t*, std::size_t, std::uint32_t, std::int32_t) const noexcept>;

namespace isolation_impl {

struct PinAttempt {
    std::error_code ec;
    std::int32_t target_cpu;
};

[[nodiscard]] inline auto try_pin(cpu_set_t* mask, std::size_t mask_size, std::int32_t target_cpu) noexcept
    -> PinAttempt {
    /**
     * @brief Prevent out-of-bounds writes on sparse CPU topologies where active CPU IDs exceed mask capacity.
     */
    if (target_cpu < 0 || toUInt(target_cpu) >= mask_size * 8) {
        return PinAttempt { .ec = std::make_error_code(std::errc::invalid_argument), .target_cpu = target_cpu };
    }
    CPU_ZERO_S(mask_size, mask);
    CPU_SET_S(toUInt(target_cpu), mask_size, mask);
    const auto res = posix::expect_success<posix::error_style::pthreads>(
        ::pthread_setaffinity_np(::pthread_self(), mask_size, mask));
    return PinAttempt { .ec = res ? std::error_code {} : res.error(), .target_cpu = target_cpu };
}

[[nodiscard]] inline auto sync_workers(const WorkerSyncCallback& worker_sync, const cpu_set_t* mask,
    std::size_t mask_size, std::uint32_t num_configured_cpus, std::int32_t target_cpu)
    -> std::expected<bool, IsolationError> {
    if (!worker_sync) { return false; }
    return worker_sync(mask, mask_size, num_configured_cpus, target_cpu).transform([]() { return true; });
}

[[nodiscard]] inline auto sync_isolation(cpu_set_t* mask, std::size_t mask_size, std::uint32_t num_configured_cpus,
    const std::int32_t initial_cpu, const WorkerSyncCallback& worker_sync)
    -> std::expected<SyncResult, IsolationError> {
    const auto first_attempt = try_pin(mask, mask_size, initial_cpu);
    if (first_attempt.ec) {
        return std::unexpected(IsolationError { .ec = first_attempt.ec, .context = "pthread_setaffinity_np" });
    }

    /**
     * @brief Mitigate scheduling race conditions (TOCTOU) where the OS
     * migrates the thread immediately after or during the initial pin.
     */
    const auto final_cpu = posix::expect_result<posix::error_style::posix>(::sched_getcpu());
    if (!final_cpu) { return std::unexpected(IsolationError { .ec = final_cpu.error(), .context = "sched_getcpu" }); }

    const auto attempt
        = (*final_cpu != first_attempt.target_cpu) ? try_pin(mask, mask_size, *final_cpu) : first_attempt;
    if (attempt.ec) {
        return std::unexpected(IsolationError { .ec = attempt.ec, .context = "pthread_setaffinity_np" });
    }

    const auto worker_res = sync_workers(worker_sync, mask, mask_size, num_configured_cpus, attempt.target_cpu);
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
    const std::uint32_t max_cpu_needed
        = num_configured_cpus > current_cpu_val ? num_configured_cpus : current_cpu_val + 1;
    const std::size_t mask_size = CPU_ALLOC_SIZE(max_cpu_needed);
    const auto raw_mask         = posix::expect_result<posix::error_style::pointer>(CPU_ALLOC(max_cpu_needed));
    if (!raw_mask) { return std::unexpected(IsolationError { .ec = raw_mask.error(), .context = "CPU_ALLOC" }); }
    cpu_set_ptr mask { *raw_mask };

    return isolation_impl::sync_isolation(mask.get(), mask_size, num_configured_cpus, *current_cpu, worker_sync);
}

} // namespace affinity

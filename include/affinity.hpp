/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "numeric_cast.hpp"
#include "utils.hpp"

#include <expected>
#include <functional>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <string>

namespace affinity {

using cpu_set_ptr = std::unique_ptr<cpu_set_t, decltype([](cpu_set_t* p) noexcept { CPU_FREE(p); })>;

/**
 * @brief Thread and process CPU affinity manager.
 *
 * Exposes orchestrators to restrict execution to specific cores
 * and align kernel-side worker pools to prevent cache thrashing.
 */
class CoreIsolator {
public:
    CoreIsolator()                               = delete;
    CoreIsolator(const CoreIsolator&)            = delete;
    CoreIsolator& operator=(const CoreIsolator&) = delete;
    CoreIsolator(CoreIsolator&&)                 = delete;
    CoreIsolator& operator=(CoreIsolator&&)      = delete;

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
    using WorkerSyncCallback = std::move_only_function<std::expected<void, std::string>(
        const cpu_set_t*, std::size_t, std::uint32_t, std::int32_t) const noexcept>;

private:
    struct PinAttempt {
        bool success;
        std::int32_t target_cpu;
    };

    [[nodiscard]] static auto try_pin(cpu_set_t* mask, std::size_t mask_size, std::int32_t target_cpu) noexcept
        -> PinAttempt {
        CPU_ZERO_S(mask_size, mask);
        CPU_SET_S(toUInt(target_cpu), mask_size, mask);
        const bool success = ::pthread_setaffinity_np(::pthread_self(), mask_size, mask) == 0;
        return PinAttempt { .success = success, .target_cpu = target_cpu };
    }

    [[nodiscard]] static auto sync_workers(const WorkerSyncCallback& worker_sync, bool main_is_ok, cpu_set_t* mask,
        std::size_t mask_size, std::uint32_t num_configured_cpus, std::int32_t target_cpu)
        -> std::expected<bool, std::string> {
        if (!worker_sync || !main_is_ok) { return false; }
        CPU_ZERO_S(mask_size, mask);
        CPU_SET_S(toUInt(target_cpu), mask_size, mask);
        return worker_sync(mask, mask_size, num_configured_cpus, target_cpu).transform([]() { return true; });
    }

    [[nodiscard]] static auto sync_isolation(cpu_set_t* mask, std::size_t mask_size, std::uint32_t num_configured_cpus,
        WorkerSyncCallback& worker_sync) -> std::expected<SyncResult, std::string> {
        const std::int32_t initial_cpu = ::sched_getcpu();
        if (initial_cpu < 0) { return SyncResult { .num_cpus = num_configured_cpus }; }

        auto [pin_ok, target_cpu] = try_pin(mask, mask_size, initial_cpu);

        /**
         * @brief Mitigate scheduling race conditions (TOCTOU) where the OS
         * migrates the thread immediately after or during the initial pin.
         */
        const std::int32_t final_cpu = ::sched_getcpu();
        if (final_cpu < 0) {
            return SyncResult { .main_thread_ok = pin_ok, .target_cpu = target_cpu, .num_cpus = num_configured_cpus };
        }
        if (final_cpu != initial_cpu) {
            auto [retry_ok, retry_cpu] = try_pin(mask, mask_size, final_cpu);
            pin_ok                     = retry_ok;
            target_cpu                 = retry_cpu;
            if (!pin_ok) {
                return SyncResult {
                    .main_thread_ok = pin_ok, .target_cpu = target_cpu, .num_cpus = num_configured_cpus
                };
            }
        }

        const auto worker_res = sync_workers(worker_sync, pin_ok, mask, mask_size, num_configured_cpus, target_cpu);
        if (!worker_res) { return std::unexpected(worker_res.error()); }

        return SyncResult { .main_thread_ok = pin_ok,
            .kernel_worker_ok               = *worker_res,
            .target_cpu                     = target_cpu,
            .num_cpus                       = num_configured_cpus };
    }

public:
    /**
     * @brief Restrict execution of the calling thread to a single stabilized core.
     *
     * Stabilizes affinity by performing verified pinning and propagating the resulting
     * mask to asynchronous kernel worker pools.
     *
     * @param worker_sync Callback to align kernel workers with the stabilized core.
     * @return Results of the isolation and worker synchronization attempt.
     */
    static auto execute_strict_isolation(WorkerSyncCallback worker_sync = nullptr) noexcept
        -> std::expected<SyncResult, std::string> {
        const std::int64_t processors_available = ::sysconf(_SC_NPROCESSORS_CONF);
        if (processors_available < 1) { return SyncResult {}; }

        const std::uint32_t num_configured_cpus = toUInt(processors_available);
        const std::size_t mask_size             = CPU_ALLOC_SIZE(num_configured_cpus);
        cpu_set_ptr mask { CPU_ALLOC(num_configured_cpus) };
        if (!mask) { return SyncResult { .num_cpus = num_configured_cpus }; }

        return sync_isolation(mask.get(), mask_size, num_configured_cpus, worker_sync);
    }
};

} // namespace affinity

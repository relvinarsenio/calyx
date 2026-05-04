/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "numeric_cast.hpp"
#include "scope.hpp"
#include "utils.hpp"

#include <expected>
#include <format>
#include <functional>
#include <pthread.h>
#include <sched.h>
#include <string>

namespace affinity {

/**
 * @brief Utility for managing thread and process CPU affinity.
 *
 * Provides high-level abstractions for pinning threads to specific cores
 * and synchronizing kernel-side workers (e.g., io_uring) to maintain
 * cache locality and minimize scheduling jitter.
 */
class CoreIsolator {
public:
    /**
     * @brief Result of an affinity synchronization operation.
     */
    struct SyncResult {
        bool main_thread_ok     = false;
        bool kernel_worker_ok   = false;
        std::int32_t active_cpu = -1;
        std::uint32_t num_cpus  = 0;
    };

    /**
     * @brief Callback type for kernel-specific worker synchronization.
     *
     * Accepts a pointer to a cpu_set_t and its allocated size (in bytes),
     * allowing support for systems with >1024 cores.
     */
    using WorkerSyncCallback = std::move_only_function<std::expected<void, std::string>(
        const cpu_set_t*, std::size_t, std::uint32_t, std::int32_t) const noexcept>;

    /**
     * @brief Enforce strict single-core isolation for the calling thread.
     *
     * Dynamically allocates a CPU mask to support large multi-core systems (>1024 cores),
     * locks the thread using RAII safety via native scope_exit, and synchronizes workers.
     *
     * @param worker_sync Optional callback for kernel worker synchronization.
     * @return SyncResult containing the setup status.
     */
    static auto enforce_strict_isolation(WorkerSyncCallback worker_sync = nullptr) noexcept
        -> std::expected<SyncResult, std::string> {
        const std::int64_t processors_available = ::sysconf(_SC_NPROCESSORS_CONF);
        if (processors_available < 1) { return SyncResult {}; }

        const std::uint32_t num_configured_cpus = toUInt(processors_available);
        const std::size_t mask_size             = CPU_ALLOC_SIZE(num_configured_cpus);
        cpu_set_t* const mask                   = CPU_ALLOC(num_configured_cpus);
        if (!mask) { return SyncResult { .num_cpus = num_configured_cpus }; }

        scope_exit free_mask { [mask] { CPU_FREE(mask); } };

        const auto sync_logic
            = [mask, mask_size, num_configured_cpus, &worker_sync]() -> std::expected<SyncResult, std::string> {
            bool main_is_ok                 = false;
            std::int32_t current_active_cpu = -1;

            const auto apply_pin_internal
                = [&main_is_ok, &current_active_cpu, mask, mask_size](std::int32_t cpu) noexcept -> bool {
                current_active_cpu = cpu;
                CPU_ZERO_S(mask_size, mask);
                CPU_SET_S(toUInt(cpu), mask_size, mask);
                if (::pthread_setaffinity_np(::pthread_self(), mask_size, mask) != 0) {
                    main_is_ok = false;
                    return false;
                }
                main_is_ok = true;
                return true;
            };

            const std::int32_t initial_target_cpu = ::sched_getcpu();
            if (initial_target_cpu < 0) { return SyncResult { .num_cpus = num_configured_cpus }; }

            /** @brief Initial pinning attempt */
            apply_pin_internal(initial_target_cpu);

            /** @brief Anti-TOCTOU Re-verification */
            const std::int32_t final_cpu = ::sched_getcpu();
            if (final_cpu < 0) {
                return SyncResult {
                    .main_thread_ok = main_is_ok, .active_cpu = current_active_cpu, .num_cpus = num_configured_cpus
                };
            }
            if (final_cpu != initial_target_cpu) {
                if (!apply_pin_internal(final_cpu)) {
                    return SyncResult {
                        .main_thread_ok = main_is_ok, .active_cpu = current_active_cpu, .num_cpus = num_configured_cpus
                    };
                }
            }

            const auto kernel_worker_res = [&worker_sync, main_is_ok, mask, mask_size, num_configured_cpus,
                                               current_active_cpu]() -> std::expected<bool, std::string> {
                if (!worker_sync || !main_is_ok) { return false; }
                CPU_ZERO_S(mask_size, mask);
                CPU_SET_S(toUInt(current_active_cpu), mask_size, mask);
                return worker_sync(mask, mask_size, num_configured_cpus, current_active_cpu).transform([]() {
                    return true;
                });
            }();

            if (!kernel_worker_res) { return std::unexpected(kernel_worker_res.error()); }

            return SyncResult { .main_thread_ok = main_is_ok,
                .kernel_worker_ok               = *kernel_worker_res,
                .active_cpu                     = current_active_cpu,
                .num_cpus                       = num_configured_cpus };
        };
        return sync_logic();
    }
};

} // namespace affinity

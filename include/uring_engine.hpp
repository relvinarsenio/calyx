/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "aligned_buffer.hpp"
#include "file_descriptor.hpp"
#include "latency_histogram.hpp"
#include "posix.hpp"
#include "posix_error.hpp"
#include "scope.hpp"
#include "utils.hpp"

#include <bit>
#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <system_error>
#include <variant>
#include <vector>

#if !defined(__linux__)
#error "Calyx requires a Linux operating system for native io_uring support."
#endif

#if __has_include(<liburing.h>)
#include <liburing.h>
#else
#error "Calyx requires liburing. Please install liburing-dev or use the build-static.sh toolchain."
#endif

namespace uring {

enum class ConfigError {
    QueueDepthNotPositive,
    BlockSizeNotPositive,
    AlignmentNotPowerOfTwo,
    AlignmentOverflow,
};

enum class AllocationError {
    WriteBufSizeOverflow,
    WriteBufAlignOverflow,
    WriteBufOom,
    ReadBufAlignOverflow,
    ReadBufOom,
    WriteBlockAlignOverflow,
    ReadBlockAlignOverflow,
    WriteMemStrideOverflow,
    ReadMemStrideOverflow,
    TotalTestSizeOverflow,
};

enum class ExecutionError {
    Timeout,
    WriteStalled,
    UnexpectedEof,
    FileRegistrationConflict,
};

enum class LogicError {
    RetrySlotOverflow,
    CqeTagOutOfBounds,
    CompletedBlocksExceedSubmitted,
    RetrySlotsExceedActiveRequests,
    FailedToGetSqeForTimer,
};

[[nodiscard]] constexpr std::string_view error_string(ConfigError err) noexcept {
    switch (err) {
        using enum ConfigError;
        case QueueDepthNotPositive:
            return "Queue depth must be positive";
        case BlockSizeNotPositive:
            return "Block size must be positive";
        case AlignmentNotPowerOfTwo:
            return "Alignment must be a power of two";
        case AlignmentOverflow:
            return "Block size combination results in alignment overflow";
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::string_view error_string(AllocationError err) noexcept {
    switch (err) {
        using enum AllocationError;
        case WriteBufSizeOverflow:
            return "Overflow in write buffer total size calculation";
        case WriteBufAlignOverflow:
            return "Overflow in write buffer allocation alignment";
        case WriteBufOom:
            return "Failed to allocate aligned write buffer";
        case ReadBufAlignOverflow:
            return "Overflow in read buffer allocation alignment";
        case ReadBufOom:
            return "Failed to allocate read partitions";
        case WriteBlockAlignOverflow:
            return "Overflow in write block size alignment";
        case ReadBlockAlignOverflow:
            return "Overflow in read block size alignment";
        case WriteMemStrideOverflow:
            return "Overflow in write memory stride alignment";
        case ReadMemStrideOverflow:
            return "Overflow in read memory stride alignment";
        case TotalTestSizeOverflow:
            return "Overflow in total test size alignment";
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::string_view error_string(ExecutionError err) noexcept {
    switch (err) {
        using enum ExecutionError;
        case Timeout:
            return "Disk benchmark timeout";
        case WriteStalled:
            return "Write operation stalled (0 bytes written)";
        case UnexpectedEof:
            return "Unexpected EOF (0 bytes read)";
        case FileRegistrationConflict:
            return "File registration conflict";
    }
    std::unreachable();
}

[[nodiscard]] constexpr std::string_view error_string(LogicError err) noexcept {
    switch (err) {
        using enum LogicError;
        case RetrySlotOverflow:
            return "Retry slot overflow";
        case CqeTagOutOfBounds:
            return "CQE tag out of bounds";
        case CompletedBlocksExceedSubmitted:
            return "Completed blocks exceed submitted blocks";
        case RetrySlotsExceedActiveRequests:
            return "Retry slots exceed active requests";
        case FailedToGetSqeForTimer:
            return "Failed to get SQE for timer";
    }
    std::unreachable();
}

struct InterruptError {};

using UringError = std::variant<ConfigError, AllocationError, ExecutionError, LogicError, std::error_code,
    InterruptError, posix::SysCallError>;

[[nodiscard]] std::string get_error_string(const UringError& err);

[[nodiscard]] inline auto make_unexpected(UringError err) {
    return std::unexpected<UringError>(std::move(err));
}

struct PhaseRunStats {
    std::chrono::duration<double> elapsed {};
    std::uint64_t io_bytes  = 0;
    std::uint64_t total_ios = 0;
    metrics::LatencyHistogram histogram;
};

struct IoContext {
    const posix::file_descriptor& fd;
    std::span<std::byte> write_buffer;
    std::span<AlignedBuffer> read_buffers;
    std::stop_token stop;
    std::reference_wrapper<const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const>>
        progress_cb;
    std::reference_wrapper<const std::move_only_function<bool() const noexcept>> interrupt_cb;
    std::uint64_t total_blocks {};
    std::uint64_t block_size {};
    std::uint64_t mem_stride {};
    std::uint64_t total_bytes {};
    std::string_view label;
    std::uint16_t queue_depth {};
};

struct IoRequest {
    std::uint64_t offset {};
    std::size_t remaining {};
    std::size_t total_len {};
    iovec iov;
    std::uint64_t start_cycles = 0;
};

enum class IoPath : std::uint8_t {
    Fixed,
    Plain,
    Vector
};

struct ProbedIoPaths {
    IoPath write_path = IoPath::Plain;
    IoPath read_path  = IoPath::Plain;
};

template <typename T>
concept ProberStrategy = requires(T& t, std::size_t count, bool keep) {
    { t.probe_register_buffers(count, keep) } -> std::same_as<std::expected<void, std::error_code>>;
};

class ResourceProber {
protected:
    struct ProbeBounds {
        std::size_t low, high;
    };

    [[nodiscard]] static bool is_buffer_register_resource_error(std::error_code ret) noexcept {
        return is_one_of<ENOMEM, E2BIG>(ret.value());
    }

    /**
     * @brief Performs an exponential backoff search to find a successful probe point.
     */
    template <typename Self>
        requires ProberStrategy<Self>
    [[nodiscard]] std::expected<std::size_t, std::error_code> find_successful_probe_floor(
        this Self& self, std::size_t high_fail) noexcept {
        if (high_fail == 0) [[unlikely]] { return std::unexpected(posix::make_error(EINVAL)); }

        struct State {
            std::optional<std::expected<std::size_t, std::error_code>> result;
            std::size_t probe;
        };

        const std::size_t iterations = toSize(std::bit_width(high_fail));

        const auto final_state = std::ranges::fold_left(std::views::iota(0uz, iterations),
            State { .result = std::nullopt, .probe = high_fail },
            [&self](const State& state, const auto) noexcept -> State {
                if (state.result) { return state; }
                const std::size_t next_probe = state.probe / 2uz;
                const auto res               = self.probe_register_buffers(next_probe, false);
                if (res) { return State { .result = next_probe, .probe = next_probe }; }
                if (!is_buffer_register_resource_error(res.error()) || next_probe == 0) {
                    return State { .result = std::unexpected(res.error()), .probe = next_probe };
                }
                return State { .result = std::nullopt, .probe = next_probe };
            });

        return final_state.result.value_or(std::unexpected(posix::make_error(EINVAL)));
    }

    /**
     * @brief Evaluates a single iteration step of the buffer registration bisection search.
     *
     * @details Isolates search state transitions into a stateless monadic flow to enforce
     *          monotonicity invariants and prevent undefined behavior. Supports early exit
     *          to avoid redundant system calls when the boundaries converge, and applies
     *          upward midpoint rounding to guarantee loop termination when retaining successful candidates.
     */
    template <typename Self>
        requires ProberStrategy<Self>
    [[nodiscard]] static auto probe_bisection_step(Self& self,
        std::expected<ProbeBounds, std::error_code> state) noexcept -> std::expected<ProbeBounds, std::error_code> {
        if (!state) { return state; }
        const ProbeBounds bounds = *state;
        if (bounds.low >= bounds.high) { return bounds; }

        const std::size_t diff = safe_sub(bounds.high, bounds.low).value_or(0uz);
        const std::size_t half = safe_add(diff, 1uz).value_or(0uz) / 2uz;
        const std::size_t mid  = safe_add(bounds.low, half).value_or(0uz);

        const auto probe_res = self.probe_register_buffers(mid, false);
        if (probe_res) { return ProbeBounds { mid, bounds.high }; }

        const std::error_code err = probe_res.error();
        if (!is_buffer_register_resource_error(err)) { return std::unexpected(err); }
        return ProbeBounds { bounds.low, safe_sub(mid, 1uz).value_or(0uz) };
    }

    /**
     * @brief Determines the maximum registerable I/O buffer count through bisection.
     *
     * @details Coordinates search boundaries between a confirmed success floor and a
     *          known failure limit to converge on the optimal memory-lock footprint.
     *          Once the optimal count is identified, it persistently registers the buffer table
     *          to transition the engine to high-performance registered-buffer I/O.
     */
    template <typename Self>
        requires ProberStrategy<Self>
    [[nodiscard]] std::expected<std::size_t, std::error_code> probe_max_read_count(
        this Self& self, std::size_t low_success, std::size_t high_fail) noexcept {
        if (high_fail == 0) [[unlikely]] {
            return self.probe_register_buffers(low_success, true).transform([low_success]() noexcept {
                return low_success;
            });
        }

        const std::size_t iterations = toSize(std::bit_width(safe_sub(high_fail, low_success).value_or(0uz)));

        return std::ranges::fold_left(std::views::iota(0uz, iterations),
            std::expected<ProbeBounds, std::error_code> {
                ProbeBounds { low_success, safe_sub(high_fail, 1uz).value_or(0uz) } },
            [&self](const auto& state, const auto) noexcept { return probe_bisection_step(self, state); })
            .and_then([&self](ProbeBounds bounds) noexcept {
                return self.probe_register_buffers(bounds.low, true).transform([bounds]() noexcept {
                    return bounds.low;
                });
            });
    }
};

class UringRing {
    io_uring ring_ {};
    bool init_ = false;
    std::error_code init_error_ {};
    bool ring_fd_registered_ = false;

    [[nodiscard]] std::expected<void, std::error_code> initialize_ring(
        std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu) noexcept;

public:
    explicit UringRing(std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu = std::nullopt);
    ~UringRing();

    UringRing(const UringRing&)            = delete;
    UringRing& operator=(const UringRing&) = delete;
    UringRing(UringRing&&)                 = delete;
    UringRing& operator=(UringRing&&)      = delete;

    [[nodiscard]] bool is_valid() const noexcept { return init_; }
    [[nodiscard]] std::error_code get_error() const noexcept { return init_error_; }
    [[nodiscard]] io_uring* get_ring() noexcept { return &ring_; }
    [[nodiscard]] bool is_ring_fd_registered() const noexcept { return ring_fd_registered_; }
    void set_ring_fd_registered(bool registered) noexcept { ring_fd_registered_ = registered; }
};

class UringProber {
public:
    [[nodiscard]] ProbedIoPaths probe_io_paths(io_uring* ring) noexcept;
};

class UringFileRegistrar {
    std::optional<posix::file_descriptor::native_handle_type> registered_handle_ = std::nullopt;

public:
    [[nodiscard]] auto register_file(io_uring* ring, const posix::file_descriptor& fd_wrapper) noexcept
        -> std::expected<void, std::error_code>;
    void unregister_file(io_uring* ring) noexcept;
    [[nodiscard]] std::optional<posix::file_descriptor::native_handle_type> registered_handle() const noexcept {
        return registered_handle_;
    }
};

class UringTimeoutController {
public:
    static constexpr std::uint64_t kTimerTag  = UINT64_MAX; ///< user_data sentinel for the timeout SQE.
    static constexpr std::uint64_t kCancelTag = UINT64_MAX - 1; ///< user_data sentinel for the cancel SQE.

    [[nodiscard]] static std::chrono::nanoseconds calculate_smart_timeout_ns(
        const metrics::LatencyHistogram& hist) noexcept;

    [[nodiscard]] static constexpr __kernel_timespec to_kernel_timespec(std::chrono::nanoseconds duration) noexcept {
        const auto secs  = std::chrono::duration_cast<std::chrono::seconds>(duration);
        const auto nsecs = duration - secs;
        return { .tv_sec = secs.count(), .tv_nsec = nsecs.count() };
    }

    [[nodiscard]] bool is_timer_armed() const noexcept { return timer_armed_; }
    void set_timer_armed(bool armed) noexcept { timer_armed_ = armed; }

    /** @brief Sets the per-wait timeout and returns a stable reference for the kernel API. */
    [[nodiscard]] __kernel_timespec& prepare_wait_timeout(std::chrono::nanoseconds duration) noexcept {
        wait_ts_ = to_kernel_timespec(duration);
        return wait_ts_;
    }

    [[nodiscard]] std::expected<void, UringError> arm_timeout_timer(io_uring* ring);
    void drain_pending_timer(io_uring* ring, std::chrono::nanoseconds timeout) noexcept;

private:
    __kernel_timespec timeout_ts_ {};
    __kernel_timespec wait_ts_ {};
    __kernel_timespec drain_ts_ {};
    bool timer_armed_ = false;
};

class UringEngine {
    friend class BufferRegistrar;
    friend class UringIoSubmitter;
    friend class UringCqeProcessor;

public:
    using IoPath = uring::IoPath;

    struct LoopState {
        std::uint64_t submitted             = 0;
        std::uint64_t completed             = 0;
        std::uint64_t bytes_completed       = 0;
        std::uint64_t io_completed          = 0;
        std::uint64_t offset                = 0;
        std::span<std::uint16_t> free_slots = {};
        std::span<std::uint64_t> deltas     = {};
        std::uint16_t free_count            = 0;
        std::uint16_t delta_count           = 0;
        bool interrupt                      = false;

        void reset_free_slots(std::uint16_t count) noexcept {
            std::ranges::iota(free_slots.subspan(0, count), std::uint16_t { 0 });
            free_count = count;
        }
    };

    struct IoPrepContext {
        const IoContext& io;
        bool is_write = false;
    };

private:
    UringRing ring_;
    UringProber prober_;
    UringFileRegistrar file_registrar_;
    UringTimeoutController timeout_controller_;

    [[nodiscard]] static std::uint32_t determine_wait_count(
        std::size_t in_kernel, std::uint32_t batch_size, const LoopState& state, const IoContext& ctx) noexcept;

    IoPath write_path_                   = IoPath::Plain;
    IoPath read_path_                    = IoPath::Plain;
    std::size_t read_buffers_registered_ = 0;

    std::vector<IoRequest> requests_;
    std::vector<std::uint16_t> retry_slots_ {};
    std::vector<std::uint16_t> free_slots_ {};
    std::vector<std::uint64_t> deltas_ {};
    std::vector<iovec> registered_iovecs_ {};
    std::vector<io_uring_cqe*> cqe_buffer_ {};
    std::size_t retry_count_ = 0;
    std::error_code buffer_register_error_ {};
    metrics::LatencyHistogram hist_ {};

public:
    explicit UringEngine(std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu = std::nullopt);

    UringEngine(const UringEngine&)            = delete;
    UringEngine& operator=(const UringEngine&) = delete;
    UringEngine(UringEngine&&)                 = delete;
    UringEngine& operator=(UringEngine&&)      = delete;

    ~UringEngine();

    [[nodiscard]] bool is_valid() const noexcept { return ring_.is_valid(); }
    [[nodiscard]] std::error_code get_error() const noexcept { return ring_.get_error(); }
    [[nodiscard]] std::error_code get_buffer_register_error() const noexcept { return buffer_register_error_; }

    [[nodiscard]] auto register_worker_affinity(const cpu_set_t* mask, std::size_t size) noexcept
        -> std::expected<void, UringError> {
        if (!ring_.is_valid()) { return std::unexpected(UringError { posix::make_error(EINVAL) }); }
        return posix::expect_success<posix::error_style::linux_internal>(
            ::io_uring_register_iowq_aff(ring_.get_ring(), size, mask))
            .transform_error([](std::error_code ec) { return UringError { ec }; });
    }

    [[nodiscard]] std::expected<void, UringError> register_buffers(
        std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) noexcept;

    [[nodiscard]] std::expected<PhaseRunStats, UringError> submit_and_wait(const IoContext& ctx, bool is_write);
};

class UringIoSubmitter {
public:
    static std::expected<void, UringError> submit_batch(
        UringEngine& engine, const IoContext& ctx, bool is_write, UringEngine::LoopState& state);
    static void prepare_io_sqe(UringEngine& engine, io_uring_sqe* sqe, const UringEngine::IoPrepContext& prep,
        IoRequest& req, std::uint16_t idx) noexcept;

private:
    [[nodiscard]] static IoPath determine_io_path(
        const UringEngine& engine, const UringEngine::IoPrepContext& prep, std::uint16_t idx) noexcept;
};

class UringCqeProcessor {
public:
    [[nodiscard]] static bool is_retryable_wait_error(std::int32_t rc) noexcept {
        return is_one_of<ETIME, EINTR, EAGAIN, EBUSY>(rc);
    }

    static std::expected<void, UringError> wait_for_submission(
        UringEngine& engine, const IoContext& ctx, std::uint32_t wait_nr);
    static std::expected<void, UringError> process_completions(
        UringEngine& engine, const IoContext& ctx, bool is_write, UringEngine::LoopState& state);
    static std::expected<void, UringError> handle_completion(
        UringEngine& engine, const io_uring_cqe* cqe, bool is_write, UringEngine::LoopState& state);
    static std::expected<void, UringError> finalize_cqe(
        UringEngine& engine, const io_uring_cqe* cqe, bool is_write, std::uint16_t idx, UringEngine::LoopState& state);
    static std::expected<void, UringError> queue_retry_slot(UringEngine& engine, std::uint16_t idx) noexcept;
    [[nodiscard]] static std::expected<std::uint16_t, UringError> resolve_cqe_slot(
        const UringEngine& engine, std::uint64_t tag) noexcept;
    static void drain_all_completions(
        UringEngine& engine, const IoContext& ctx, bool is_write, UringEngine::LoopState& state) noexcept;
    [[nodiscard]] static std::expected<void, std::error_code> wait_one_cqe(UringEngine& engine) noexcept;
};

class BufferRegistrar : private ResourceProber {
    friend class ResourceProber;
    friend class UringEngine;

    enum class MemlockBudgetState {
        Limited,
        Unlimited,
        Unknown,
    };

    struct MemlockBudget {
        MemlockBudgetState state;
        std::uint64_t bytes {};
    };

    UringEngine& engine_;
    using IoPath = UringEngine::IoPath;

    explicit BufferRegistrar(UringEngine& engine) noexcept
        : engine_(engine) {}

public:
    [[nodiscard]] std::expected<void, std::error_code> register_buffers(
        std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) noexcept;

private:
    [[nodiscard]] std::expected<MemlockBudget, std::error_code> check_root_memlock_budget() const noexcept;
    [[nodiscard]] static MemlockBudget map_rlimit_to_budget(const rlimit& limit) noexcept;
    [[nodiscard]] static constexpr std::size_t compile_time_iovec_limit() noexcept {
        std::size_t limit = std::numeric_limits<std::size_t>::max();
#ifdef UIO_MAXIOV
        limit = std::min<std::size_t>(limit, UIO_MAXIOV);
#endif
#ifdef IOV_MAX
        if constexpr (IOV_MAX > 0) { limit = std::min<std::size_t>(limit, IOV_MAX); }
#endif
        return limit;
    }
    [[nodiscard]] static std::size_t max_registerable_iovecs() noexcept;
    [[nodiscard]] bool has_valid_buffer_registration_inputs(std::span<std::byte> write_buf) const noexcept;
    [[nodiscard]] std::expected<std::size_t, std::error_code> compute_memlock_read_limit(
        std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) const noexcept;
    [[nodiscard]] std::expected<std::size_t, std::error_code> compute_target_read_count(
        std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) const noexcept;
    void populate_registered_iovecs(
        std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs, std::size_t read_count) noexcept;
    void set_buffer_registration_state(std::size_t read_count) noexcept;
    void fail_buffer_registration(std::error_code error_code) noexcept;
    [[nodiscard]] std::expected<std::size_t, std::error_code> probe_adaptive_read_registration(
        std::size_t target_read_count) noexcept;
    [[nodiscard]] std::expected<void, std::error_code> probe_register_buffers(
        std::size_t read_count, bool keep_registered) noexcept;
    [[nodiscard]] MemlockBudget pinned_memory_budget() const noexcept;
};

} // namespace uring

using uring::IoContext;
using uring::PhaseRunStats;
using uring::UringEngine;

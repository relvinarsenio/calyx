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
#include "random_engine.hpp"
#include "scope.hpp"
#include "utils.hpp"

#include <bit>
#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <random>
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
    metrics::LatencyHistogram histogram {};
};

template <typename T>
concept IoBufferSpan = std::ranges::contiguous_range<T>
    && (std::same_as<std::remove_cvref_t<std::ranges::range_value_t<T>>, std::byte>
        || std::same_as<std::remove_cvref_t<std::ranges::range_value_t<T>>, memory::AlignedBuffer>);

struct IoRequest {
    iovec iov {};
    std::uint64_t offset {};
    std::size_t remaining {};
    std::size_t total_len {};
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

/**
 * @brief Dynamic execution state maintained across submission and completion queues.
 */
struct IoTrackerState {
    std::uint64_t submitted {};
    std::uint64_t completed {};
    std::uint64_t bytes_completed {};
    std::uint64_t io_completed {};
    /** @brief Linear byte cursor used exclusively for sequential stream progression. */
    std::uint64_t offset {};
    std::uint16_t free_count {};
    std::uint16_t delta_count {};
    bool interrupt {};
};

struct NextBlock {
    std::uint64_t offset {};
    std::size_t len {};
};

/**
 * @brief Layout geometry and block progression for sequential linear-scan I/O.
 */
struct SequentialLayout {
    std::uint64_t file_bytes {};
    std::uint64_t block_size {};
    std::uint64_t mem_stride {};
    std::uint16_t queue_depth {};

    [[nodiscard]] constexpr std::uint64_t total_bytes() const noexcept { return file_bytes; }

    [[nodiscard]] constexpr std::uint64_t total_blocks() const noexcept {
        return block_size > 0uz ? (file_bytes / block_size) : 0uz;
    }

    [[nodiscard]] constexpr NextBlock next(IoTrackerState& state) const noexcept {
        const auto len = toSize(std::ranges::min(block_size, file_bytes - state.offset));
        return { .offset = std::exchange(state.offset, state.offset + len), .len = len };
    }
};

/**
 * @brief Layout geometry and pseudo-random block selection for Random 4K / IOPS.
 */
struct RandomLayout {
    std::uint64_t file_size {};
    std::uint64_t block_size {};
    std::uint64_t mem_stride {};
    std::uint64_t total_ops {};
    std::uint16_t queue_depth {};

    [[nodiscard]] constexpr std::uint64_t addressable_blocks() const noexcept {
        return block_size > 0uz ? (file_size / block_size) : 0uz;
    }

    [[nodiscard]] constexpr std::uint64_t total_bytes() const noexcept { return total_ops * block_size; }

    [[nodiscard]] constexpr std::uint64_t total_blocks() const noexcept { return total_ops; }

    template <std::uniform_random_bit_generator Prng>
    [[nodiscard]] constexpr NextBlock next(Prng& prng) const noexcept {
        const auto blocks    = addressable_blocks();
        const auto block_idx = (blocks > 0uz) ? (prng() % blocks) : 0uz;
        return { .offset = block_idx * block_size, .len = toSize(block_size) };
    }
};

/**
 * @brief Unified Type-Driven IoLayout with ergonomic named factories.
 */
class IoLayout {
public:
    using LayoutVariant = std::variant<SequentialLayout, RandomLayout>;

    constexpr explicit IoLayout(SequentialLayout seq) noexcept
        : variant_(seq) {}
    constexpr explicit IoLayout(RandomLayout rnd) noexcept
        : variant_(rnd) {}

    [[nodiscard]] static constexpr IoLayout sequential(
        std::uint64_t total_bytes, std::uint64_t block_size, std::uint64_t mem_stride, std::uint16_t qd) noexcept {
        return IoLayout(SequentialLayout {
            .file_bytes  = total_bytes,
            .block_size  = block_size,
            .mem_stride  = mem_stride,
            .queue_depth = qd,
        });
    }

    [[nodiscard]] static constexpr IoLayout random(std::uint64_t file_size, std::uint64_t block_size,
        std::uint64_t mem_stride, std::uint64_t total_ops, std::uint16_t qd) noexcept {
        return IoLayout(RandomLayout {
            .file_size   = file_size,
            .block_size  = block_size,
            .mem_stride  = mem_stride,
            .total_ops   = total_ops,
            .queue_depth = qd,
        });
    }

    [[nodiscard]] constexpr std::uint64_t block_size() const noexcept {
        return std::visit([](const auto& l) noexcept { return l.block_size; }, variant_);
    }
    [[nodiscard]] constexpr std::uint64_t mem_stride() const noexcept {
        return std::visit([](const auto& l) noexcept { return l.mem_stride; }, variant_);
    }
    [[nodiscard]] constexpr std::uint64_t total_bytes() const noexcept {
        return std::visit([](const auto& l) noexcept { return l.total_bytes(); }, variant_);
    }
    [[nodiscard]] constexpr std::uint64_t total_blocks() const noexcept {
        return std::visit([](const auto& l) noexcept { return l.total_blocks(); }, variant_);
    }
    [[nodiscard]] constexpr std::uint16_t queue_depth() const noexcept {
        return std::visit([](const auto& l) noexcept { return l.queue_depth; }, variant_);
    }

    [[nodiscard]] constexpr const LayoutVariant& variant() const noexcept { return variant_; }

private:
    LayoutVariant variant_;
};

struct IoObserver {
    std::string_view label {};
    std::stop_token stop {};
    std::reference_wrapper<const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const>>
        progress_cb;
    std::reference_wrapper<const std::move_only_function<bool() const noexcept>> interrupt_cb;
};

template <IoBufferSpan BufferType> struct IoContextBase {
    BufferType buffers;
    const posix::file_descriptor& fd;
    IoLayout layout;
    IoObserver observer;
};

struct WriteContext final : IoContextBase<std::span<const std::byte>> {
    static constexpr bool is_write_op = true;

    [[nodiscard]] constexpr auto get_slice(std::uint16_t idx, std::size_t done, std::size_t remaining) const noexcept {
        const auto subspan_offset = toSize(idx) * layout.mem_stride() + done;
        return buffers.subspan(subspan_offset, remaining);
    }
};

struct ReadContext final : IoContextBase<std::span<memory::AlignedBuffer>> {
    static constexpr bool is_write_op = false;

    [[nodiscard]] constexpr auto get_slice(std::uint16_t idx, std::size_t done, std::size_t remaining) const noexcept {
        return buffers[idx].span().subspan(done, remaining);
    }
};

template <typename Context>
concept IoContext = std::same_as<std::remove_cvref_t<Context>, WriteContext>
    || std::same_as<std::remove_cvref_t<Context>, ReadContext>;

/**
 * @brief Generic probing algorithms for adaptive kernel resource registration.
 *
 * @details Provides stateless free function templates parametrised on a callable
 *          probe function, eliminating the need for inheritance or friend access.
 */
namespace probing {

struct ProbeBounds {
    std::size_t low = 0, high = 0;
};

[[nodiscard]] inline bool is_resource_error(std::error_code ret) noexcept {
    return is_one_of<ENOMEM, E2BIG>(ret.value());
}

/**
 * @brief Performs an exponential backoff search to find a successful probe point.
 */
template <std::invocable<std::size_t, bool> ProbeFn>
[[nodiscard]] std::expected<std::size_t, std::error_code> find_successful_probe_floor(
    ProbeFn&& probe_fn, std::size_t high_fail) noexcept {
    if (high_fail == 0) [[unlikely]] { return std::unexpected(posix::make_error(EINVAL)); }

    struct State {
        std::optional<std::expected<std::size_t, std::error_code>> result;
        std::size_t probe;
    };

    const std::size_t iterations = toSize(std::bit_width(high_fail));

    const auto final_state = std::ranges::fold_left(std::views::iota(0uz, iterations),
        State { .result = std::nullopt, .probe = high_fail },
        [&probe_fn](const State& state, const auto) noexcept -> State {
            if (state.result) { return state; }
            const std::size_t next_probe = state.probe / 2uz;
            const auto res               = probe_fn(next_probe, false);
            if (res) { return State { .result = next_probe, .probe = next_probe }; }
            if (!is_resource_error(res.error()) || next_probe == 0) {
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
template <std::invocable<std::size_t, bool> ProbeFn>
[[nodiscard]] std::expected<ProbeBounds, std::error_code> bisection_step(
    ProbeFn&& probe_fn, std::expected<ProbeBounds, std::error_code> state) noexcept {
    if (!state) { return state; }
    const ProbeBounds bounds = *state;
    if (bounds.low >= bounds.high) { return bounds; }

    const std::size_t diff = safe_sub(bounds.high, bounds.low).value_or(0uz);
    const std::size_t half = safe_add(diff, 1uz).value_or(0uz) / 2uz;
    const std::size_t mid  = safe_add(bounds.low, half).value_or(0uz);

    const auto probe_res = probe_fn(mid, false);
    if (probe_res) { return ProbeBounds { mid, bounds.high }; }

    const std::error_code err = probe_res.error();
    if (!is_resource_error(err)) { return std::unexpected(err); }
    return ProbeBounds { bounds.low, safe_sub(mid, 1uz).value_or(0uz) };
}

/**
 * @brief Determines the maximum registerable count through bisection.
 *
 * @details Coordinates search boundaries between a confirmed success floor and a
 *          known failure limit to converge on the optimal memory-lock footprint.
 *          Once the optimal count is identified, it persistently registers the buffer table
 *          to transition the engine to high-performance registered-buffer I/O.
 */
template <std::invocable<std::size_t, bool> ProbeFn>
[[nodiscard]] std::expected<std::size_t, std::error_code> probe_max_count(
    ProbeFn&& probe_fn, std::size_t low_success, std::size_t high_fail) noexcept {
    if (high_fail == 0) [[unlikely]] {
        return probe_fn(low_success, true).transform([low_success]() noexcept { return low_success; });
    }

    const std::size_t iterations = toSize(std::bit_width(safe_sub(high_fail, low_success).value_or(0uz)));

    return std::ranges::fold_left(std::views::iota(0uz, iterations),
        std::expected<ProbeBounds, std::error_code> {
            ProbeBounds { low_success, safe_sub(high_fail, 1uz).value_or(0uz) } },
        [&probe_fn](const auto& state, const auto) noexcept { return bisection_step(probe_fn, state); })
        .and_then([&probe_fn](ProbeBounds bounds) noexcept {
            return probe_fn(bounds.low, true).transform([bounds]() noexcept { return bounds.low; });
        });
}

} // namespace probing

class UringRing {
    io_uring ring_ {};
    bool ring_fd_registered_ = false;

    explicit UringRing(io_uring ring, bool registered) noexcept;

public:
    UringRing() noexcept;
    ~UringRing();
    UringRing(UringRing&& other) noexcept;
    UringRing& operator=(UringRing&& other) noexcept;
    UringRing(const UringRing&)            = delete;
    UringRing& operator=(const UringRing&) = delete;

    [[nodiscard]] static std::expected<UringRing, std::error_code> create(
        std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu = std::nullopt) noexcept;

    [[nodiscard]] io_uring* get_ring() noexcept { return &ring_; }
    [[nodiscard]] const io_uring* get_ring() const noexcept { return &ring_; }
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
    __kernel_timespec timeout_ts_ {};
    bool timer_armed_ = false;

    /**
     * @note Initialized to zero. The first call to get_cached_timeout() (when completed == 0)
     * will immediately populate this with calculate_smart_timeout_ns().
     */
    std::chrono::nanoseconds cached_timeout_ { std::chrono::nanoseconds::zero() };
    std::uint64_t last_recalc_completed_ = 0;

public:
    static constexpr std::uint64_t kTimerTag  = UINT64_MAX; ///< user_data sentinel for the timeout SQE.
    static constexpr std::uint64_t kCancelTag = UINT64_MAX - 1; ///< user_data sentinel for the cancel SQE.

    [[nodiscard]] std::chrono::nanoseconds get_cached_timeout(
        const metrics::LatencyHistogram& hist, std::uint64_t completed) noexcept {
        /**
         * @brief Minimum completions required for p99.9 to have statistical significance.
         * @details Based on binomial tail expectation (n >= 1/(1-p)), establishing a mathematical floor.
         */
        constexpr std::uint64_t kMinTailDefinedSamples = 1000;

        /**
         * @brief Recalculate at ~25% relative sample growth for consistent standard error.
         * @details Based on asymptotic quantile variance (Mosteller 1946): Var(x̂_p) ≈ p(1-p)/(n·f(x_p)²).
         *          A relative growth ratio (Δ) cancels the unknown density f(x_p): SE(n(1+Δ))/SE(n) = 1/√(1+Δ).
         *          For a 10% SE reduction target, Δ ≈ 25% (divisor 4).
         */
        constexpr std::uint64_t kGrowthDivisor = 4;

        const std::uint64_t dynamic_interval
            = std::max(kMinTailDefinedSamples / kGrowthDivisor, last_recalc_completed_ / kGrowthDivisor);

        if (completed < last_recalc_completed_ || completed - last_recalc_completed_ >= dynamic_interval
            || completed == 0) {
            cached_timeout_        = calculate_smart_timeout_ns(hist);
            last_recalc_completed_ = completed;
        }
        return cached_timeout_;
    }

    [[nodiscard]] static std::chrono::nanoseconds calculate_smart_timeout_ns(
        const metrics::LatencyHistogram& hist) noexcept;

    [[nodiscard]] static constexpr __kernel_timespec to_kernel_timespec(std::chrono::nanoseconds duration) noexcept {
        const auto secs  = std::chrono::duration_cast<std::chrono::seconds>(duration);
        const auto nsecs = duration - secs;
        return { .tv_sec = secs.count(), .tv_nsec = nsecs.count() };
    }

    [[nodiscard]] bool is_timer_armed() const noexcept { return timer_armed_; }
    void set_timer_armed(bool armed) noexcept { timer_armed_ = armed; }

    [[nodiscard]] std::expected<void, UringError> arm_timeout_timer(io_uring* ring);
    void drain_pending_timer(io_uring* ring, std::chrono::nanoseconds timeout) noexcept;
};

class IoTracker {
    std::vector<IoRequest> requests_ {};
    std::vector<std::uint16_t> retry_slots_ {};
    std::vector<std::uint16_t> free_slots_ {};
    std::vector<std::uint64_t> deltas_ {};
    std::size_t retry_count_ = 0;
    IoTrackerState state_ {};
    metrics::LatencyHistogram hist_ {};

public:
    explicit IoTracker(std::uint16_t queue_depth);

    [[nodiscard]] IoTrackerState& state() noexcept { return state_; }
    [[nodiscard]] const IoTrackerState& state() const noexcept { return state_; }
    [[nodiscard]] IoRequest& request(std::uint16_t idx) noexcept { return requests_[idx]; }
    [[nodiscard]] metrics::LatencyHistogram& histogram() noexcept { return hist_; }
    [[nodiscard]] const metrics::LatencyHistogram& histogram() const noexcept { return hist_; }

    void reset(std::uint16_t queue_depth) noexcept;
    void finalize_deltas() noexcept;

    [[nodiscard]] std::span<const std::uint16_t> get_retry_slots() const noexcept;
    void consume_retries(std::size_t count) noexcept;

    [[nodiscard]] std::span<const std::uint16_t> get_free_slots(std::size_t limit) const noexcept;
    void consume_free_slots(std::size_t count) noexcept;

    [[nodiscard]] std::expected<void, UringError> queue_retry_slot(std::uint16_t idx) noexcept;
    [[nodiscard]] std::expected<std::uint16_t, UringError> resolve_cqe_slot(std::uint64_t tag) const noexcept;
    void record_delta(std::uint64_t start_cycles) noexcept;
    void push_free_slot(std::uint16_t idx) noexcept;
};

struct IoPathState {
    IoPath write                        = IoPath::Plain;
    IoPath read                         = IoPath::Plain;
    std::size_t read_buffers_registered = 0;
};

struct UringSharedState {
    UringRing& ring;
    UringFileRegistrar& file_registrar;
    UringTimeoutController& timeout_controller;
    IoPathState& path_state;
};

class SubmissionQueue {
    UringSharedState shared_state_;
    IoTracker& tracker_;
    prng::Xoshiro256PlusPlus prng_ {};

    [[nodiscard]] NextBlock next_block(const IoLayout& layout, IoTrackerState& state) noexcept;

    template <IoContext Context>
    void prepare_io_sqe(io_uring_sqe* sqe, const Context& ctx, IoRequest& req, std::uint16_t idx) noexcept;
    template <IoContext Context> [[nodiscard]] IoPath determine_io_path(std::uint16_t idx) const noexcept;

public:
    SubmissionQueue(UringSharedState shared_state, IoTracker& tracker) noexcept;

    template <IoContext Context> [[nodiscard]] std::expected<void, UringError> submit_batch(const Context& ctx);
};

class CompletionQueue {
    UringSharedState shared_state_;
    IoTracker& tracker_;
    std::vector<io_uring_cqe*> cqe_buffer_ {};

    [[nodiscard]] static bool is_retryable_wait_error(std::int32_t rc) noexcept;

    template <IoContext Context>
    [[nodiscard]] std::expected<void, UringError> handle_completion(const io_uring_cqe* cqe);

    template <IoContext Context>
    [[nodiscard]] std::expected<void, UringError> finalize_cqe(const io_uring_cqe* cqe, std::uint16_t idx);
    [[nodiscard]] std::expected<void, std::error_code> wait_one_cqe() noexcept;

public:
    CompletionQueue(UringSharedState shared_state, IoTracker& tracker, std::uint16_t queue_depth);

    template <IoContext Context>
    [[nodiscard]] std::expected<void, UringError> wait_for_submission(const Context& ctx, std::uint32_t wait_nr);

    template <IoContext Context> [[nodiscard]] std::expected<void, UringError> process_completions(const Context& ctx);

    template <IoContext Context> void drain_all_completions(const Context& ctx) noexcept;
};

class UringEventLoop {
    UringSharedState shared_state_;
    IoTracker tracker_;
    SubmissionQueue sq_;
    CompletionQueue cq_;

public:
    UringEventLoop(UringSharedState shared_state, std::uint16_t queue_depth);

    template <IoContext Context> [[nodiscard]] std::expected<PhaseRunStats, UringError> execute(const Context& ctx);
};

/** @brief Outcome of a buffer registration attempt. */
struct BufferRegistrationResult {
    std::size_t read_buffers_registered = 0;
    IoPath write_path                   = IoPath::Plain;
    IoPath read_path                    = IoPath::Plain;
    std::error_code error {};
};

class UringEngine {
    UringRing ring_;
    UringFileRegistrar file_registrar_;
    UringTimeoutController timeout_controller_;
    IoPathState path_state_ {};
    UringEventLoop event_loop_;

    std::vector<iovec> registered_iovecs_ {};
    std::error_code buffer_register_error_ {};

    UringProber prober_;

    explicit UringEngine(UringRing ring);
    void apply_registration_result(const BufferRegistrationResult& result) noexcept;

public:
    using IoPath = uring::IoPath;

    [[nodiscard]] static std::expected<UringEngine, std::error_code> create(
        std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu = std::nullopt);

    UringEngine(const UringEngine&)            = delete;
    UringEngine& operator=(const UringEngine&) = delete;

    UringEngine(UringEngine&& other);
    UringEngine& operator=(UringEngine&& other);

    ~UringEngine();

    [[nodiscard]] std::error_code get_buffer_register_error() const noexcept { return buffer_register_error_; }

    [[nodiscard]] auto register_worker_affinity(const cpu_set_t* mask, std::size_t size) noexcept
        -> std::expected<void, UringError>;

    [[nodiscard]] std::expected<void, UringError> register_buffers(
        std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs) noexcept;

    [[nodiscard]] std::expected<PhaseRunStats, UringError> execute_write(const WriteContext& ctx);
    [[nodiscard]] std::expected<PhaseRunStats, UringError> execute_read(const ReadContext& ctx);
};

class BufferRegistrar {
    enum class MemlockBudgetState {
        Limited,
        Unlimited,
        Unknown,
    };

    struct MemlockBudget {
        std::uint64_t bytes {};
        MemlockBudgetState state {};
    };

    io_uring* ring_ = nullptr;
    std::span<iovec> iovecs_ {};

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
        std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs) const noexcept;
    [[nodiscard]] std::expected<std::size_t, std::error_code> compute_target_read_count(
        std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs) const noexcept;
    void populate_registered_iovecs(
        std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs, std::size_t read_count) noexcept;
    [[nodiscard]] std::expected<std::size_t, std::error_code> probe_adaptive_read_registration(
        std::size_t target_read_count) noexcept;
    [[nodiscard]] std::expected<void, std::error_code> probe_register_buffers(
        std::size_t read_count, bool keep_registered) noexcept;
    [[nodiscard]] MemlockBudget pinned_memory_budget() const noexcept;

public:
    explicit BufferRegistrar(io_uring* ring, std::span<iovec> iovecs) noexcept
        : ring_(ring)
        , iovecs_(iovecs) {}

    [[nodiscard]] BufferRegistrationResult register_buffers(
        std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs) noexcept;
};

} // namespace uring

using uring::IoContextBase;
using uring::IoLayout;
using uring::NextBlock;
using uring::PhaseRunStats;
using uring::RandomLayout;
using uring::ReadContext;
using uring::SequentialLayout;
using uring::UringEngine;
using uring::WriteContext;

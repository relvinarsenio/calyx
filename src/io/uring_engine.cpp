/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "uring_engine.hpp"

#include "config.hpp"
#include "utils.hpp"

namespace uring {

[[nodiscard]] std::string get_error_string(const UringError& err) {
    return std::visit(overloaded { [](const std::error_code& e) -> std::string { return e.message(); },
                          [](const InterruptError&) -> std::string { return std::string { ::config::kInterruptMsg }; },
                          [](const posix::SysCallError& e) -> std::string { return format_sys_error(e.ec, e.context); },
                          [](const auto& e) -> std::string { return std::string { error_string(e) }; } },
        err);
}

std::expected<UringRing, std::error_code> UringRing::create(
    std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu) noexcept {
    constexpr std::uint32_t kCoop      = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
    constexpr std::uint32_t kBase      = kCoop | IORING_SETUP_SINGLE_ISSUER;
    constexpr std::uint32_t kNoSqArray = kBase | IORING_SETUP_NO_SQARRAY;
    constexpr std::uint32_t kModern    = kNoSqArray | IORING_SETUP_DEFER_TASKRUN;

    /**
     * @brief Fallback ladder ordered from most-optimized to most-compatible.
     * - DEFER_TASKRUN requires SINGLE_ISSUER and is incompatible with SQPOLL.
     * - NO_SQARRAY (kernel >= 6.6) is incompatible with SQPOLL.
     * - SQPOLL entries omit SINGLE_ISSUER (the SQ poll thread is a second issuer).
     */
    constexpr auto kFlagSets = std::to_array<std::uint32_t>(
        { kModern, kNoSqArray, kCoop | IORING_SETUP_SQPOLL, IORING_SETUP_SQPOLL, kBase, kCoop, 0u });

    io_uring ring {};
    std::int32_t last_ret = -1;

    for (const auto flags : kFlagSets) {
        struct io_uring_params params {};
        params.flags = flags;

        if (sq_thread_cpu.has_value() && (flags & IORING_SETUP_SQPOLL) != 0u) {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = toUInt(*sq_thread_cpu);
        }

        last_ret = io_uring_queue_init_params(queue_depth, &ring, &params);
        if (last_ret == 0) { break; }
    }

    const auto init_res = posix::expect_success<posix::error_style::linux_internal>(last_ret);
    if (!init_res) { return std::unexpected(init_res.error()); }

    const bool registered
        = posix::expect_success<posix::error_style::linux_internal>(io_uring_register_ring_fd(&ring)).has_value();

    if constexpr (requires(io_uring_clock_register arg) {
                      { io_uring_register_clock(&ring, &arg) } -> std::convertible_to<int>;
                  }) {
        io_uring_clock_register clock_arg {};
        clock_arg.clockid = CLOCK_MONOTONIC;
        io_uring_register_clock(&ring, &clock_arg);
    }

    return UringRing { ring, registered };
}

UringRing::UringRing(io_uring ring, bool registered) noexcept
    : ring_(ring)
    , ring_fd_registered_(registered) {}

UringRing::UringRing() noexcept {
    ring_.ring_fd = -1;
}

UringRing::~UringRing() {
    if (ring_fd_registered_) { io_uring_unregister_ring_fd(&ring_); }
    if (ring_.ring_fd >= 0) { io_uring_queue_exit(&ring_); }
}

UringRing::UringRing(UringRing&& other) noexcept
    : ring_(other.ring_)
    , ring_fd_registered_(std::exchange(other.ring_fd_registered_, false)) {
    other.ring_.ring_fd = -1;
}

UringRing& UringRing::operator=(UringRing&& other) noexcept {
    if (this == &other) [[unlikely]] { return *this; }

    if (ring_fd_registered_) { io_uring_unregister_ring_fd(&ring_); }
    if (ring_.ring_fd >= 0) { io_uring_queue_exit(&ring_); }

    ring_               = other.ring_;
    ring_fd_registered_ = std::exchange(other.ring_fd_registered_, false);
    other.ring_.ring_fd = -1;

    return *this;
}

ProbedIoPaths UringProber::probe_io_paths(io_uring* ring) noexcept {
    struct io_uring_probe* probe = io_uring_get_probe_ring(ring);
    scope_exit cleanup_probe { [probe]() noexcept {
        if (probe) { io_uring_free_probe(probe); }
    } };

    if (!probe) {
        /**
         * @brief Probe unsupported (kernel < 5.6).
         * Default to Vector as a safe fallback for pre-5.6 kernels, as READV/WRITEV exist since 5.1.
         */
        return { IoPath::Vector, IoPath::Vector };
    }

    IoPath write_path = IoPath::Plain;
    IoPath read_path  = IoPath::Plain;
    if (io_uring_opcode_supported(probe, IORING_OP_WRITE) == 0) { write_path = IoPath::Vector; }
    if (io_uring_opcode_supported(probe, IORING_OP_READ) == 0) { read_path = IoPath::Vector; }
    return { write_path, read_path };
}

auto UringFileRegistrar::register_file(io_uring* ring, const posix::file_descriptor& fd_wrapper) noexcept
    -> std::expected<void, std::error_code> {
    const auto fd = fd_wrapper.native_handle();
    if (registered_handle_) {
        if (*registered_handle_ == fd) { return {}; }
        return std::unexpected(posix::make_error(EBUSY));
    }

    return posix::expect_success<posix::error_style::linux_internal>(io_uring_register_files(ring, &fd, 1))
        .transform([this, fd]() { registered_handle_ = fd; });
}

void UringFileRegistrar::unregister_file(io_uring* ring) noexcept {
    if (!registered_handle_) { return; }
    io_uring_unregister_files(ring);
    registered_handle_ = std::nullopt;
}

/**
 * @brief Dynamic timeout derivation.
 *
 * Calculate an adaptive wait timeout based on historical latency metrics (p99) to
 * avoid indefinite blocking while remaining responsive to variable I/O speeds.
 * Includes a generous multiplier to absorb jitter and prevent spurious ETIME wakeups.
 */
std::chrono::nanoseconds UringTimeoutController::calculate_smart_timeout_ns(
    const metrics::LatencyHistogram& hist) noexcept {
    using namespace std::chrono_literals;
    constexpr auto kMinTimeoutNs          = std::chrono::nanoseconds(10ms);
    constexpr auto kMaxTimeoutNs          = std::chrono::nanoseconds(3s);
    constexpr auto kOsTimerResolutionPad  = std::chrono::nanoseconds(1ms);
    constexpr auto kJitterToleranceFactor = 4z; ///< Jacobson/Karels variance multiplier (RFC 6298) to absorb I/O jitter

    static const double cycles_to_ns = tsc::calibrate();
    const metrics::LatencyAnalyzer analyzer { hist, cycles_to_ns };
    const auto p99_9_ns = std::chrono::nanoseconds(toLong(analyzer.percentile(99.9).count()));

    const auto jitter_margin  = p99_9_ns * kJitterToleranceFactor;
    const auto target_timeout = jitter_margin + kOsTimerResolutionPad;

    return std::clamp(target_timeout, kMinTimeoutNs, kMaxTimeoutNs);
}

std::expected<void, UringError> UringTimeoutController::arm_timeout_timer(io_uring* ring) {
    timeout_ts_ = to_kernel_timespec(::config::kDiskBenchmarkMaxDuration);

    return posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(ring))
        .transform_error([](auto) { return UringError { LogicError::FailedToGetSqeForTimer }; })
        .and_then([this, ring](io_uring_sqe* sqe) {
            io_uring_prep_timeout(sqe, &timeout_ts_, 0, 0);
            io_uring_sqe_set_data64(sqe, kTimerTag);

            return posix::expect_success<posix::error_style::linux_internal>(io_uring_submit(ring))
                .transform_error(
                    [](auto err) { return UringError { posix::SysCallError { err, "io_uring_submit (timer)" } }; });
        })
        .transform([this]() { timer_armed_ = true; });
}

void UringTimeoutController::drain_pending_timer(io_uring* ring, std::chrono::nanoseconds timeout) noexcept {
    if (!timer_armed_) { return; }
    scope_exit reset_timer { [this]() noexcept { timer_armed_ = false; } };

    const auto sqe_res
        = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(ring)).or_else([ring](auto) {
              io_uring_submit(ring);
              return posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(ring));
          });

    if (!sqe_res) { return; }
    io_uring_sqe* sqe = *sqe_res;

    io_uring_prep_cancel64(sqe, kTimerTag, 0);
    io_uring_sqe_set_data64(sqe, kCancelTag);

    if (!posix::expect_success<posix::error_style::linux_internal>(io_uring_submit(ring))) { return; }

    io_uring_cqe* cqe          = nullptr;
    bool cancel_seen           = false;
    bool timer_seen            = false;
    __kernel_timespec drain_ts = to_kernel_timespec(timeout);

    while (!(timer_seen && cancel_seen)) {
        if (!posix::expect_success<posix::error_style::linux_internal>(
                io_uring_wait_cqe_timeout(ring, &cqe, &drain_ts))) {
            break;
        }

        const auto tag = io_uring_cqe_get_data64(cqe);
        const auto res = cqe->res; ///< Save before cqe_seen releases the slot.
        io_uring_cqe_seen(ring, cqe);
        if (tag == kTimerTag) { timer_seen = true; }
        if (tag != kCancelTag) { continue; }

        cancel_seen = true;

        /** @brief Distinguish genuine cancel failures from benign race outcomes. */
        const auto is_cancel_error
            = [](std::int32_t result) noexcept { return result < 0 && !is_one_of<-ENOENT, -EALREADY>(result); };
        if (is_cancel_error(res)) { print_warning(std::format("io_uring cancel failed: {}", res)); }
    }
}

IoTracker::IoTracker(std::uint16_t queue_depth) {
    requests_.resize(queue_depth);
    retry_slots_.resize(queue_depth);
    free_slots_.resize(queue_depth);
    deltas_.resize(queue_depth);
    reset(queue_depth);
}

void IoTracker::reset(std::uint16_t queue_depth) noexcept {
    state_       = IoTrackerState {};
    hist_        = {};
    retry_count_ = 0;
    std::ranges::iota(free_slots_.begin(), free_slots_.begin() + queue_depth, std::uint16_t { 0 });
    state_.free_count = queue_depth;
}

void IoTracker::finalize_deltas() noexcept {
    std::ranges::for_each(
        deltas_ | std::views::take(state_.delta_count), [this](const auto delta) noexcept { hist_.add(delta); });
    state_.delta_count = 0;
}

std::span<const std::uint16_t> IoTracker::get_retry_slots() const noexcept {
    return std::span { retry_slots_ }.subspan(0, retry_count_);
}

void IoTracker::consume_retries(std::size_t count) noexcept {
    retry_count_ -= count;
}

std::span<const std::uint16_t> IoTracker::get_free_slots(std::size_t limit) const noexcept {
    return std::span { free_slots_ }
        .subspan(0, state_.free_count)
        .subspan(0, std::min(limit, toSize(state_.free_count)));
}

void IoTracker::consume_free_slots(std::size_t count) noexcept {
    state_.free_count -= count;
}

std::expected<void, UringError> IoTracker::queue_retry_slot(std::uint16_t idx) noexcept {
    if (retry_count_ >= retry_slots_.size()) { return make_unexpected(LogicError::RetrySlotOverflow); }
    retry_slots_[retry_count_] = idx;
    ++retry_count_;
    return {};
}

std::expected<std::uint16_t, UringError> IoTracker::resolve_cqe_slot(std::uint64_t tag) const noexcept {
    const std::uint16_t idx = toUShort(tag);
    if (toSize(idx) >= requests_.size()) [[unlikely]] { return make_unexpected(LogicError::CqeTagOutOfBounds); }
    return idx;
}

void IoTracker::record_delta(std::uint64_t start_cycles) noexcept {
    const auto end_cycles = tsc::rdtscp_ordered();
    if (end_cycles > start_cycles) [[likely]] {
        deltas_[state_.delta_count] = end_cycles - start_cycles;
        ++state_.delta_count;
    }
}

void IoTracker::push_free_slot(std::uint16_t idx) noexcept {
    free_slots_[state_.free_count] = idx;
    ++state_.free_count;
}

SubmissionQueue::SubmissionQueue(UringSharedState shared_state, IoTracker& tracker) noexcept
    : shared_state_(shared_state)
    , tracker_(tracker) {}

template <IoContext Context> std::expected<void, UringError> SubmissionQueue::submit_batch(const Context& ctx) {
    /**
     * @brief Core Submission Loop
     *
     * 1. Re-submit any I/O requests that encountered transient EAGAIN/EINTR errors.
     * 2. Populate available SQEs with new blocks from the context until the queue depth
     *    or total block limit is reached.
     */
    auto& state                   = tracker_.state();
    const auto retry_slots_view   = tracker_.get_retry_slots() | std::views::reverse;
    std::size_t retries_submitted = 0;

    for (const auto idx : retry_slots_view) {
        const auto sqe_res
            = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(shared_state_.ring.get_ring()));
        if (!sqe_res) { break; }
        io_uring_sqe* sqe = *sqe_res;

        prepare_io_sqe(sqe, ctx, tracker_.request(idx), idx);
        retries_submitted += 1;
    }
    tracker_.consume_retries(retries_submitted);

    const std::size_t queue_in_flight = safe_sub(state.submitted, state.completed).value_or(0uz);
    const std::size_t queue_limit
        = ctx.layout.queue_depth > queue_in_flight ? ctx.layout.queue_depth - queue_in_flight : 0uz;
    const std::size_t blocks_remaining
        = ctx.layout.total_blocks > state.submitted ? ctx.layout.total_blocks - state.submitted : 0uz;
    const std::size_t limit = std::min({ blocks_remaining, queue_limit, toSize(state.free_count) });

    const auto free_slots_view     = tracker_.get_free_slots(limit) | std::views::reverse;
    std::size_t submitted_in_batch = 0;

    for (const auto idx : free_slots_view) {
        if (state.interrupt || ctx.observer.stop.stop_requested()) {
            state.interrupt = true;
            tracker_.consume_free_slots(submitted_in_batch);
            return std::unexpected(UringError { InterruptError {} });
        }

        const auto sqe_res
            = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(shared_state_.ring.get_ring()));
        if (!sqe_res) { break; }
        io_uring_sqe* sqe = *sqe_res;

        const std::uint64_t remaining_bytes = ctx.layout.total_bytes - state.offset;
        const std::size_t len               = toSize(std::ranges::min(ctx.layout.block_size, remaining_bytes));
        tracker_.request(idx)               = { {}, state.offset, len, len, 0 };

        prepare_io_sqe(sqe, ctx, tracker_.request(idx), idx);

        state.offset += len;
        submitted_in_batch += 1;
    }
    tracker_.consume_free_slots(submitted_in_batch);
    state.submitted += submitted_in_batch;

    return {};
}

template <IoContext Context> IoPath SubmissionQueue::determine_io_path(std::uint16_t idx) const noexcept {
    constexpr bool is_write = std::remove_cvref_t<Context>::is_write_op;
    if constexpr (is_write) { return shared_state_.path_state.write; }
    if (shared_state_.path_state.read != IoPath::Fixed) { return shared_state_.path_state.read; }
    return (toSize(idx) < shared_state_.path_state.read_buffers_registered) ? IoPath::Fixed : IoPath::Plain;
}

template <IoContext Context>
void SubmissionQueue::prepare_io_sqe(
    io_uring_sqe* sqe, const Context& ctx, IoRequest& req, std::uint16_t idx) noexcept {
    constexpr bool is_write = std::remove_cvref_t<Context>::is_write_op;
    const std::size_t done  = req.total_len - req.remaining;

    const auto slice = ctx.get_slice(idx, done, req.remaining);

    /** @brief Use registered file index (0) if available, otherwise raw FD. */
    const posix::file_descriptor::native_handle_type fd
        = shared_state_.file_registrar.registered_handle() ? 0 : ctx.fd.native_handle();

    const IoPath current_path    = determine_io_path<Context>(idx);
    const std::uint32_t len      = toUInt(slice.size());
    const std::int32_t buf_index = is_write ? 0 : idx + 1;

    switch (current_path) {
        case IoPath::Fixed:
            if constexpr (is_write) {
                io_uring_prep_write_fixed(sqe, fd, slice.data(), len, req.offset, buf_index);
            } else {
                io_uring_prep_read_fixed(sqe, fd, slice.data(), len, req.offset, buf_index);
            }
            break;
        case IoPath::Vector:
            if constexpr (is_write) {
                /** @brief POSIX iovec struct is shared and lacks constness. Safe to const_cast for writev. */
                req.iov = { .iov_base = const_cast<void*>(static_cast<const void*>(slice.data())),
                    .iov_len          = slice.size() };
                io_uring_prep_writev(sqe, fd, &req.iov, 1, req.offset);
            } else {
                req.iov = { .iov_base = slice.data(), .iov_len = slice.size() };
                io_uring_prep_readv(sqe, fd, &req.iov, 1, req.offset);
            }
            break;
        case IoPath::Plain:
            if constexpr (is_write) {
                io_uring_prep_write(sqe, fd, slice.data(), len, req.offset);
            } else {
                io_uring_prep_read(sqe, fd, slice.data(), len, req.offset);
            }
            break;
    }

    req.start_cycles = tsc::rdtsc_ordered();
    io_uring_sqe_set_data64(sqe, idx);
    /** @brief Mark as fixed file if we are using the registered table index. */
    if (shared_state_.file_registrar.registered_handle()) {
        io_uring_sqe_set_flags(sqe, sqe->flags | IOSQE_FIXED_FILE);
    }
}

CompletionQueue::CompletionQueue(UringSharedState shared_state, IoTracker& tracker, std::uint16_t queue_depth)
    : shared_state_(shared_state)
    , tracker_(tracker) {
    cqe_buffer_.resize(queue_depth);
}

bool CompletionQueue::is_retryable_wait_error(std::int32_t rc) noexcept {
    return is_one_of<ETIME, EINTR, EAGAIN, EBUSY>(rc);
}

template <IoContext Context>
std::expected<void, UringError> CompletionQueue::wait_for_submission(const Context& ctx, std::uint32_t wait_nr) {
    io_uring_cqe* cqe_ptr = nullptr;

    const auto timeout_ns
        = shared_state_.timeout_controller.get_cached_timeout(tracker_.histogram(), tracker_.state().completed);
    __kernel_timespec wait_ts = UringTimeoutController::to_kernel_timespec(timeout_ns);

    const auto res = posix::expect_success<posix::error_style::linux_internal>(
        io_uring_submit_and_wait_timeout(shared_state_.ring.get_ring(), &cqe_ptr, wait_nr, &wait_ts, nullptr));

    if (!res && !is_retryable_wait_error(toInt(res.error().value()))) {
        return std::unexpected(UringError { posix::SysCallError { res.error(), "io_uring_submit_and_wait" } });
    }

    if (ctx.observer.interrupt_cb.get()() || ctx.observer.stop.stop_requested()) {
        return std::unexpected(UringError { InterruptError {} });
    }

    return {};
}

template <IoContext Context> std::expected<void, UringError> CompletionQueue::process_completions(const Context& ctx) {
    const auto cqe_count
        = io_uring_peek_batch_cqe(shared_state_.ring.get_ring(), cqe_buffer_.data(), toUInt(cqe_buffer_.size()));
    if (cqe_count == 0) { return {}; }

    const auto cqe_span = std::span { cqe_buffer_ }.subspan(0, cqe_count);
    auto processed      = 0u;

    scope_exit advance_cq { [this, &processed]() noexcept {
        if (processed > 0) { io_uring_cq_advance(shared_state_.ring.get_ring(), processed); }
        tracker_.finalize_deltas();
    } };

    for (const auto* cqe : cqe_span) {
        if (const auto res = handle_completion<Context>(cqe); !res) {
            processed += 1;
            return res;
        }
        processed += 1;
    }

    ctx.observer.progress_cb.get()(tracker_.state().completed, ctx.layout.total_blocks, ctx.observer.label);
    return {};
}

template <IoContext Context>
std::expected<void, UringError> CompletionQueue::handle_completion(const io_uring_cqe* cqe) {
    constexpr bool is_write = std::remove_cvref_t<Context>::is_write_op;
    const auto tag          = io_uring_cqe_get_data64(cqe);
    if (tag == UringTimeoutController::kTimerTag) {
        shared_state_.timeout_controller.set_timer_armed(false);
        if (cqe->res == -ETIME) { return make_unexpected(ExecutionError::Timeout); }
        return {};
    }

    if (tag == UringTimeoutController::kCancelTag) { return {}; }

    const auto idx_res = tracker_.resolve_cqe_slot(tag);
    if (!idx_res) { return std::unexpected(idx_res.error()); }
    const std::uint16_t idx = *idx_res;

    if (is_one_of<-EAGAIN, -EINTR>(cqe->res)) { return tracker_.queue_retry_slot(idx); }

    if (is_one_of<-EINVAL, -EOPNOTSUPP, -EBADE, -EBADF>(cqe->res)
        && (is_write ? shared_state_.path_state.write : shared_state_.path_state.read) != IoPath::Vector) {
        if constexpr (is_write) {
            shared_state_.path_state.write = IoPath::Vector;
        } else {
            shared_state_.path_state.read = IoPath::Vector;
        }
        return tracker_.queue_retry_slot(idx);
    }

    return finalize_cqe<Context>(cqe, idx);
}

template <IoContext Context>
std::expected<void, UringError> CompletionQueue::finalize_cqe(const io_uring_cqe* cqe, std::uint16_t idx) {
    constexpr bool is_write = std::remove_cvref_t<Context>::is_write_op;
    const auto res_opt      = posix::expect_result<posix::error_style::linux_internal>(cqe->res);
    if (!res_opt) {
        return std::unexpected(UringError { posix::SysCallError { res_opt.error(), is_write ? "write" : "read" } });
    }
    const std::int32_t res = *res_opt;
    const auto bytes       = toSize(res);

    auto& state = tracker_.state();
    state.bytes_completed += bytes;

    if (bytes == 0) [[unlikely]] {
        return make_unexpected(is_write ? ExecutionError::WriteStalled : ExecutionError::UnexpectedEof);
    }

    auto& req = tracker_.request(idx);
    if (bytes < req.remaining) {
        req.remaining -= bytes;
        req.offset += bytes;
        return tracker_.queue_retry_slot(idx);
    }

    ++state.completed;
    ++state.io_completed;

    tracker_.record_delta(req.start_cycles);
    tracker_.push_free_slot(idx);
    return {};
}

template <IoContext Context> void CompletionQueue::drain_all_completions(const Context& ctx) noexcept {
    constexpr std::uint32_t kMaxDrainRetries = 3;

    auto& state           = tracker_.state();
    const auto drain_step = [this, &ctx, &state]() noexcept -> bool {
        const auto initial = state.completed;
        return wait_one_cqe()
            /** @brief Error from wait_one_cqe is intentionally ignored during drain. */
            .or_else([](auto) { return std::expected<void, UringError> {}; })
            .and_then([this, &ctx]() -> std::expected<void, UringError> { return process_completions(ctx); })
            .transform([&state, initial]() { return state.completed > initial || state.completed == state.submitted; })
            .value_or(false);
    };

    std::uint32_t consecutive_stalls = 0;
    while (state.completed < state.submitted) {
        if (drain_step()) [[likely]] {
            consecutive_stalls = 0;
            continue;
        }
        ++consecutive_stalls;
        if (consecutive_stalls >= kMaxDrainRetries) {
            print_warning(std::format("io_uring: drain stalled at {}/{} completions after {} retries", state.completed,
                state.submitted, kMaxDrainRetries));
            break;
        }
    }
}

std::expected<void, std::error_code> CompletionQueue::wait_one_cqe() noexcept {
    io_uring_cqe* cqe    = nullptr;
    __kernel_timespec ts = UringTimeoutController::to_kernel_timespec(config::kUringWaitTimeout);
    return posix::expect_success<posix::error_style::linux_internal>(
        ::io_uring_wait_cqe_timeout(shared_state_.ring.get_ring(), &cqe, &ts));
}

UringEventLoop::UringEventLoop(UringSharedState shared_state, std::uint16_t queue_depth)
    : shared_state_(shared_state)
    , tracker_(queue_depth)
    , sq_(shared_state_, tracker_)
    , cq_(shared_state_, tracker_, queue_depth) {}

template <IoContext Context> std::expected<PhaseRunStats, UringError> UringEventLoop::execute(const Context& ctx) {

    /**
     * @brief Register the file for I/O operations to eliminate FD instantiation overhead during submit.
     *
     * If the file descriptor is already registered or registration fails with EBUSY, we fallback to
     * traditional FD usage without aborting, ensuring robustness in varied execution contexts.
     */
    if (const auto res = shared_state_.file_registrar.register_file(shared_state_.ring.get_ring(), ctx.fd); !res) {
        if (res.error().value() == EBUSY) { return make_unexpected(ExecutionError::FileRegistrationConflict); }
        print_warning("io_uring: fixed-file registration failed, using raw fd");
    }
    scope_exit unreg_file { [this]() noexcept {
        shared_state_.file_registrar.unregister_file(shared_state_.ring.get_ring());
    } };

    tracker_.reset(ctx.layout.queue_depth);

    if (const auto arm = shared_state_.timeout_controller.arm_timeout_timer(shared_state_.ring.get_ring()); !arm) {
        return std::unexpected(arm.error());
    }

    const auto t0 = std::chrono::steady_clock::now();

    /**
     * @brief Always drain pending timer SQE, including early-return paths.
     *
     * This is required because the same UringEngine instance is reused
     * for the subsequent read phase; a stale timer CQE would corrupt the
     * next completion-processing pass.
     */
    scope_exit drain_timer { [this]() noexcept {
        if (shared_state_.timeout_controller.is_timer_armed()) {
            const auto timeout_ns
                = shared_state_.timeout_controller.get_cached_timeout(tracker_.histogram(), tracker_.state().completed);
            shared_state_.timeout_controller.drain_pending_timer(shared_state_.ring.get_ring(), timeout_ns);
        }
    } };

    /**
     * @brief Safety Drain: Ensure all in-flight I/O is reaped before destruction.
     *
     * If the benchmark is interrupted or fails early, we MUST wait for all
     * pending requests to complete (or cancel) before allowing the caller to
     * destroy the I/O buffers. Failure to do so would cause a Use-After-Free
     * when the kernel eventually writes to the (freed) buffer memory.
     */
    scope_exit drain_all { [this, &ctx]() noexcept {
        if (tracker_.state().completed < tracker_.state().submitted) { cq_.drain_all_completions(ctx); }
    } };

    /**
     * @brief Defines the fraction of the queue to wait for during batching.
     *
     * Sourced dynamically from Config::kIoBatchPercent. For example, a value of 25
     * means we wait for 25% of the queue to complete before waking up. This keeps
     * the storage controller saturated while reducing syscalls.
     */
    const std::uint32_t batch_size
        = std::max<std::uint32_t>(1, (toUInt(ctx.layout.queue_depth) * ::config::kIoBatchPercent) / 100);

    auto& state = tracker_.state();

    while (state.completed < ctx.layout.total_blocks) {
        if (const auto res = sq_.submit_batch(ctx); !res) { return std::unexpected(res.error()); }

        const auto active_requests_opt = safe_sub(state.submitted, state.completed);
        if (!active_requests_opt) [[unlikely]] { return make_unexpected(LogicError::CompletedBlocksExceedSubmitted); }
        const std::size_t active_requests = *active_requests_opt;

        const auto in_kernel_opt = safe_sub(active_requests, tracker_.get_retry_slots().size());
        if (!in_kernel_opt) [[unlikely]] { return make_unexpected(LogicError::RetrySlotsExceedActiveRequests); }
        const std::size_t in_kernel = *in_kernel_opt;

        if (active_requests == 0) { break; }

        /**
         * @brief Determines the batch wait threshold to balance throughput and system call overhead.
         *
         * Blocking wait is enforced when the queue capacity is exhausted or all blocks have been submitted,
         * avoiding CPU spinning while preserving device pipeline saturation.
         */
        const std::uint32_t wait_nr = (state.free_count == 0 || state.submitted == ctx.layout.total_blocks)
            ? std::min<std::uint32_t>(toUInt(in_kernel), batch_size)
            : 0u;

        if (wait_nr > 0) {
            if (const auto wait = cq_.wait_for_submission(ctx, wait_nr); !wait) {
                return std::unexpected(wait.error());
            }
        } else {
            if (const auto res = posix::expect_success<posix::error_style::linux_internal>(
                    io_uring_submit(shared_state_.ring.get_ring()));
                !res) {
                return std::unexpected(UringError { posix::SysCallError { res.error(), "io_uring_submit" } });
            }
        }

        const auto proc_res = cq_.process_completions(ctx);
        if (!proc_res) { return std::unexpected(proc_res.error()); }
    }

    const auto t1 = std::chrono::steady_clock::now();
    return PhaseRunStats {
        .elapsed   = t1 - t0,
        .io_bytes  = state.bytes_completed,
        .total_ios = state.io_completed,
        .histogram = tracker_.histogram(),
    };
}

std::expected<UringEngine, std::error_code> UringEngine::create(
    std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu) {
    return UringRing::create(queue_depth, sq_thread_cpu).transform([](UringRing ring) {
        return UringEngine(std::move(ring));
    });
}

UringEngine::UringEngine(UringRing ring)
    : ring_(std::move(ring))
    , event_loop_(UringSharedState { ring_, file_registrar_, timeout_controller_, path_state_ },
          toUShort(ring_.get_ring()->sq.ring_entries)) {
    const auto queue_depth = ring_.get_ring()->sq.ring_entries;
    registered_iovecs_.resize(safe_add(queue_depth, 1uz).value_or(0uz));

    const auto probed = prober_.probe_io_paths(ring_.get_ring());
    path_state_.write = probed.write_path;
    path_state_.read  = probed.read_path;
}

UringEngine::UringEngine(UringEngine&& other)
    : ring_(std::move(other.ring_))
    , file_registrar_(std::move(other.file_registrar_))
    , timeout_controller_(std::move(other.timeout_controller_))
    , path_state_(other.path_state_)
    , event_loop_(UringSharedState { ring_, file_registrar_, timeout_controller_, path_state_ },
          toUShort(ring_.get_ring()->sq.ring_entries))
    , registered_iovecs_(std::move(other.registered_iovecs_))
    , buffer_register_error_(other.buffer_register_error_)
    , prober_(std::move(other.prober_)) {
    other.path_state_.read_buffers_registered = 0;
}

UringEngine& UringEngine::operator=(UringEngine&& other) {
    if (this != &other) {
        ring_               = std::move(other.ring_);
        file_registrar_     = std::move(other.file_registrar_);
        timeout_controller_ = std::move(other.timeout_controller_);
        path_state_         = other.path_state_;

        std::destroy_at(&event_loop_);
        std::construct_at(&event_loop_, UringSharedState { ring_, file_registrar_, timeout_controller_, path_state_ },
            toUShort(ring_.get_ring()->sq.ring_entries));

        registered_iovecs_     = std::move(other.registered_iovecs_);
        buffer_register_error_ = other.buffer_register_error_;

        prober_ = std::move(other.prober_);

        other.path_state_.read_buffers_registered = 0;
    }
    return *this;
}

UringEngine::~UringEngine() {
    if (ring_.get_ring()->ring_fd < 0) { return; }
    file_registrar_.unregister_file(ring_.get_ring());
    if (path_state_.write == IoPath::Fixed || path_state_.read_buffers_registered > 0) {
        io_uring_unregister_buffers(ring_.get_ring());
    }
}

void UringEngine::apply_registration_result(const BufferRegistrationResult& result) noexcept {
    path_state_.read_buffers_registered = result.read_buffers_registered;
    path_state_.write                   = result.write_path;
    path_state_.read                    = result.read_path;
    buffer_register_error_              = result.error;
}

std::expected<void, UringError> UringEngine::register_buffers(
    std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs) noexcept {
    auto result = BufferRegistrar { ring_.get_ring(), registered_iovecs_ }.register_buffers(write_buf, read_bufs);
    apply_registration_result(result);

    if (result.error) { return std::unexpected<UringError>(result.error); }
    return {};
}

auto UringEngine::register_worker_affinity(const cpu_set_t* mask, std::size_t size) noexcept
    -> std::expected<void, UringError> {
    return posix::expect_success<posix::error_style::linux_internal>(
        io_uring_register_iowq_aff(ring_.get_ring(), size, mask))
        .transform_error(
            [](std::error_code ec) { return UringError { posix::SysCallError { ec, "io_uring_register_iowq_aff" } }; });
}

std::expected<PhaseRunStats, UringError> UringEngine::execute_write(const WriteContext& ctx) {
    return event_loop_.execute(ctx);
}

std::expected<PhaseRunStats, UringError> UringEngine::execute_read(const ReadContext& ctx) {
    return event_loop_.execute(ctx);
}

BufferRegistrationResult BufferRegistrar::register_buffers(
    std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs) noexcept {

    if (!has_valid_buffer_registration_inputs(write_buf)) {
        return BufferRegistrationResult { .error = posix::make_error(EINVAL) };
    }

    const auto target_count = compute_target_read_count(write_buf, read_bufs);
    if (!target_count) { return BufferRegistrationResult { .error = target_count.error() }; }

    populate_registered_iovecs(write_buf, read_bufs, *target_count);

    const auto read_count = probe_adaptive_read_registration(*target_count);
    if (!read_count) { return BufferRegistrationResult { .error = read_count.error() }; }

    return BufferRegistrationResult {
        .read_buffers_registered = *read_count,
        .write_path              = IoPath::Fixed,
        .read_path               = IoPath::Fixed,
    };
}

std::size_t BufferRegistrar::max_registerable_iovecs() noexcept {
    constexpr std::size_t ct_limit = compile_time_iovec_limit();
    std::size_t limit              = ct_limit;
    bool has_limit                 = (ct_limit != std::numeric_limits<std::size_t>::max());

    if constexpr (requires { _SC_IOV_MAX; }) {
        const auto runtime_limit_res     = posix::expect_result<posix::error_style::posix>(::sysconf(_SC_IOV_MAX));
        const std::int64_t runtime_limit = (runtime_limit_res && *runtime_limit_res > 0) ? *runtime_limit_res : -1L;
        if (runtime_limit > 0) {
            limit     = std::min(limit, toSize(runtime_limit));
            has_limit = true;
        }
    }

    return has_limit ? limit : 1024uz;
}

bool BufferRegistrar::has_valid_buffer_registration_inputs(std::span<std::byte> write_buf) const noexcept {
    const auto queue_depth = ring_->sq.ring_entries;
    return !write_buf.empty() && iovecs_.size() == safe_add(queue_depth, 1uz).value_or(0uz);
}

std::expected<std::size_t, std::error_code> BufferRegistrar::compute_memlock_read_limit(
    std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs) const noexcept {
    const auto memlock_budget = pinned_memory_budget();
    if (memlock_budget.state != MemlockBudgetState::Limited) { return read_bufs.size(); }

    const auto write_size = write_buf.size();
    if (memlock_budget.bytes < write_size) { return std::unexpected(posix::make_error(ENOMEM)); }

    auto remaining = safe_sub(memlock_budget.bytes, write_size).value_or(0uz);
    auto it        = std::ranges::find_if(read_bufs, [&remaining](const auto& buffer) noexcept {
        const auto buffer_size = buffer.size();
        if (remaining < buffer_size) { return true; }
        remaining = safe_sub(remaining, buffer_size).value_or(0uz);
        return false;
    });

    return toSize(std::distance(read_bufs.begin(), it));
}

std::expected<std::size_t, std::error_code> BufferRegistrar::compute_target_read_count(
    std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs) const noexcept {
    const std::size_t max_iovecs = std::min(max_registerable_iovecs(), iovecs_.size());
    if (max_iovecs == 0) { return std::unexpected(posix::make_error(E2BIG)); }

    return compute_memlock_read_limit(write_buf, read_bufs)
        .transform([max_iovecs, &read_bufs](std::size_t max_read_by_memlock) {
            const std::size_t max_read_by_iov = safe_sub(max_iovecs, 1uz).value_or(0uz);
            return std::ranges::min({ read_bufs.size(), max_read_by_iov, max_read_by_memlock });
        });
}

void BufferRegistrar::populate_registered_iovecs(
    std::span<std::byte> write_buf, std::span<memory::AlignedBuffer> read_bufs, std::size_t read_count) noexcept {
    iovecs_[0] = { .iov_base = write_buf.data(), .iov_len = write_buf.size() };

    const auto targets = iovecs_ | std::views::drop(1) | std::views::take(read_count);
    std::ranges::for_each(std::views::zip(targets, read_bufs), [](auto&& pair) noexcept {
        auto&& [dest, src] = pair;
        dest               = { .iov_base = src.data(), .iov_len = src.size() };
    });
}

std::expected<std::size_t, std::error_code> BufferRegistrar::probe_adaptive_read_registration(
    std::size_t target_read_count) noexcept {

    if (const auto initial_probe = probe_register_buffers(target_read_count, true)) {
        return target_read_count;
    } else if (const std::error_code err = initial_probe.error();
        !probing::is_resource_error(err) || target_read_count == 0) {
        return std::unexpected(err);
    }

    const auto probe_fn = [this](std::size_t count, bool keep) noexcept { return probe_register_buffers(count, keep); };

    return probing::find_successful_probe_floor(probe_fn, target_read_count)
        .and_then([&probe_fn, target_read_count](
                      std::size_t floor) { return probing::probe_max_count(probe_fn, floor, target_read_count); });
}

std::expected<void, std::error_code> BufferRegistrar::probe_register_buffers(
    std::size_t read_count, bool keep_registered) noexcept {
    const std::size_t iovec_count = safe_add(read_count, 1uz).value_or(0uz);

    const auto register_res = posix::expect_success<posix::error_style::linux_internal>(
        io_uring_register_buffers(ring_, iovecs_.data(), toUInt(iovec_count)));

    if (!register_res || keep_registered) { return register_res; }

    return posix::expect_success<posix::error_style::linux_internal>(io_uring_unregister_buffers(ring_));
}

BufferRegistrar::MemlockBudget BufferRegistrar::map_rlimit_to_budget(const struct rlimit& limit) noexcept {
    if (limit.rlim_cur == RLIM_INFINITY) {
        return MemlockBudget { .bytes = 0, .state = MemlockBudgetState::Unlimited };
    }
    return MemlockBudget { .bytes = toULong(limit.rlim_cur), .state = MemlockBudgetState::Limited };
}

BufferRegistrar::MemlockBudget BufferRegistrar::pinned_memory_budget() const noexcept {
    if (::geteuid() == 0) { return MemlockBudget { .bytes = 0, .state = MemlockBudgetState::Unlimited }; }

    return posix::get_rlimit(RLIMIT_MEMLOCK)
        .transform(map_rlimit_to_budget)
        .value_or(MemlockBudget { .bytes = 0, .state = MemlockBudgetState::Unknown });
}

} // namespace uring

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

    if (last_ret != 0) { return std::unexpected(posix::make_error(last_ret)); }

    const bool registered
        = posix::expect_success<posix::error_style::linux_internal>(io_uring_register_ring_fd(&ring)).has_value();

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
        .and_then([this, fd]() -> std::expected<void, std::error_code> {
            registered_handle_ = fd;
            return {};
        });
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
    timeout_ts_ = { .tv_sec = ::config::kDiskBenchmarkMaxSeconds, .tv_nsec = 0 };

    return posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(ring))
        .transform_error([](auto) { return UringError { LogicError::FailedToGetSqeForTimer }; })
        .and_then([this, ring](io_uring_sqe* sqe) {
            io_uring_prep_timeout(sqe, &timeout_ts_, 0, 0);
            io_uring_sqe_set_data64(sqe, kTimerTag);

            return posix::expect_success<posix::error_style::linux_internal>(io_uring_submit(ring))
                .transform_error(
                    [](auto err) { return UringError { posix::SysCallError { err, "io_uring_submit (timer)" } }; });
        })
        .and_then([this]() -> std::expected<void, UringError> {
            timer_armed_ = true;
            return {};
        });
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

SubmissionQueue::SubmissionQueue(UringRing& ring, IoTracker& tracker, UringFileRegistrar& registrar) noexcept
    : ring_(ring)
    , tracker_(tracker)
    , file_registrar_(registrar) {}

void SubmissionQueue::set_paths(IoPath write, IoPath read, std::size_t read_registered) noexcept {
    write_path_              = write;
    read_path_               = read;
    read_buffers_registered_ = read_registered;
}

template <IsIoContext Context> std::expected<void, UringError> SubmissionQueue::submit_batch(const Context& ctx) {
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
        const auto sqe_res = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(ring_.get_ring()));
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

        const auto sqe_res = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(ring_.get_ring()));
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

template <IsIoContext Context> IoPath SubmissionQueue::determine_io_path(std::uint16_t idx) const noexcept {
    constexpr bool is_write = std::remove_cvref_t<Context>::is_write_op;
    if (tracker_.state().downgrade_to_vector) { return IoPath::Vector; }
    if constexpr (is_write) { return write_path_; }
    if (read_path_ != IoPath::Fixed) { return read_path_; }
    return (toSize(idx) < read_buffers_registered_) ? IoPath::Fixed : IoPath::Plain;
}

template <IsIoContext Context>
void SubmissionQueue::prepare_io_sqe(
    io_uring_sqe* sqe, const Context& ctx, IoRequest& req, std::uint16_t idx) noexcept {
    constexpr bool is_write = std::remove_cvref_t<Context>::is_write_op;
    const std::size_t done  = req.total_len - req.remaining;

    const auto slice = ctx.get_slice(idx, done, req.remaining);

    /** @brief Use registered file index (0) if available, otherwise raw FD. */
    const posix::file_descriptor::native_handle_type fd
        = file_registrar_.registered_handle() ? 0 : ctx.fd.native_handle();

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
    if (file_registrar_.registered_handle()) { sqe->flags |= IOSQE_FIXED_FILE; }
}

CompletionQueue::CompletionQueue(
    UringRing& ring, IoTracker& tracker, UringTimeoutController& timeout, std::uint16_t queue_depth)
    : ring_(ring)
    , tracker_(tracker)
    , timeout_controller_(timeout) {
    cqe_buffer_.resize(queue_depth);
}

void CompletionQueue::set_paths(IoPath write, IoPath read) noexcept {
    write_path_ = write;
    read_path_  = read;
}

bool CompletionQueue::is_retryable_wait_error(std::int32_t rc) noexcept {
    return is_one_of<ETIME, EINTR, EAGAIN, EBUSY>(rc);
}

template <IsIoContext Context>
std::expected<void, UringError> CompletionQueue::wait_for_submission(const Context& ctx, std::uint32_t wait_nr) {
    io_uring_cqe* cqe_ptr = nullptr;

    const auto timeout_ns     = UringTimeoutController::calculate_smart_timeout_ns(tracker_.histogram());
    __kernel_timespec wait_ts = UringTimeoutController::to_kernel_timespec(timeout_ns);

    return posix::expect_success<posix::error_style::linux_internal>(
        io_uring_submit_and_wait_timeout(ring_.get_ring(), &cqe_ptr, wait_nr, &wait_ts, nullptr))
        .or_else([](std::error_code err) -> std::expected<void, std::error_code> {
            return is_retryable_wait_error(toInt(err.value())) ? std::expected<void, std::error_code> {}
                                                               : std::unexpected(err);
        })
        .transform_error(
            [](auto err) { return UringError { posix::SysCallError { err, "io_uring_submit_and_wait" } }; })
        .and_then([&ctx]() -> std::expected<void, UringError> {
            return (ctx.observer.interrupt_cb.get()() || ctx.observer.stop.stop_requested())
                ? std::unexpected(UringError { InterruptError {} })
                : std::expected<void, UringError> {};
        });
}

template <IsIoContext Context>
std::expected<void, UringError> CompletionQueue::process_completions(const Context& ctx) {
    const std::uint32_t count
        = toUInt(io_uring_peek_batch_cqe(ring_.get_ring(), cqe_buffer_.data(), toUInt(cqe_buffer_.size())));
    if (count == 0) { return {}; }

    const auto cqe_span   = std::span { cqe_buffer_ }.subspan(0, count);
    std::size_t processed = 0;

    scope_exit advance_cq { [this, &processed]() noexcept {
        if (processed > 0) { io_uring_cq_advance(ring_.get_ring(), toUInt(processed)); }
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

template <IsIoContext Context>
std::expected<void, UringError> CompletionQueue::handle_completion(const io_uring_cqe* cqe) {
    constexpr bool is_write = std::remove_cvref_t<Context>::is_write_op;
    const auto tag          = io_uring_cqe_get_data64(cqe);
    if (tag == UringTimeoutController::kTimerTag) {
        timeout_controller_.set_timer_armed(false);
        if (cqe->res == -ETIME) { return make_unexpected(ExecutionError::Timeout); }
        return {};
    }

    if (tag == UringTimeoutController::kCancelTag) { return {}; }

    const auto idx_res = tracker_.resolve_cqe_slot(tag);
    if (!idx_res) { return std::unexpected(idx_res.error()); }
    const std::uint16_t idx = *idx_res;

    if (is_one_of<-EAGAIN, -EINTR>(cqe->res)) { return tracker_.queue_retry_slot(idx); }

    if (cqe->res == -EINVAL && (is_write ? write_path_ : read_path_) != IoPath::Vector) {
        if (is_write) {
            write_path_ = IoPath::Vector;
        } else {
            read_path_ = IoPath::Vector;
        }
        tracker_.state().downgrade_to_vector = true;
        return tracker_.queue_retry_slot(idx);
    }

    return finalize_cqe<Context>(cqe, idx);
}

template <IsIoContext Context>
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

template <IsIoContext Context> void CompletionQueue::drain_all_completions(const Context& ctx) noexcept {
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
    io_uring_cqe* cqe = nullptr;
    __kernel_timespec ts { .tv_sec = 0, .tv_nsec = config::kUringWaitTimeoutNs };
    return posix::expect_success<posix::error_style::linux_internal>(
        ::io_uring_wait_cqe_timeout(ring_.get_ring(), &cqe, &ts));
}

UringEventLoop::UringEventLoop(
    UringRing& ring, UringFileRegistrar& file_registrar, UringTimeoutController& timeout, std::uint16_t queue_depth)
    : ring_(ring)
    , tracker_(queue_depth)
    , file_registrar_(file_registrar)
    , timeout_controller_(timeout)
    , sq_(ring, tracker_, file_registrar)
    , cq_(ring, tracker_, timeout, queue_depth) {}

void UringEventLoop::set_paths(IoPath write, IoPath read, std::size_t read_registered) noexcept {
    sq_.set_paths(write, read, read_registered);
    cq_.set_paths(write, read);
}

template <IsIoContext Context> std::expected<PhaseRunStats, UringError> UringEventLoop::execute(const Context& ctx) {

    /**
     * @brief Register the file for I/O operations to eliminate FD instantiation overhead during submit.
     *
     * If the file descriptor is already registered or registration fails with EBUSY, we fallback to
     * traditional FD usage without aborting, ensuring robustness in varied execution contexts.
     */
    if (const auto res = file_registrar_.register_file(ring_.get_ring(), ctx.fd); !res) {
        if (res.error().value() == EBUSY) { return make_unexpected(ExecutionError::FileRegistrationConflict); }
        print_warning("io_uring: fixed-file registration failed, using raw fd");
    }
    scope_exit unreg_file { [this]() noexcept { file_registrar_.unregister_file(ring_.get_ring()); } };

    tracker_.reset(ctx.layout.queue_depth);

    if (const auto arm = timeout_controller_.arm_timeout_timer(ring_.get_ring()); !arm) {
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
        if (timeout_controller_.is_timer_armed()) {
            const auto timeout_ns = UringTimeoutController::calculate_smart_timeout_ns(tracker_.histogram());
            timeout_controller_.drain_pending_timer(ring_.get_ring(), timeout_ns);
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
            if (const auto res
                = posix::expect_success<posix::error_style::linux_internal>(io_uring_submit(ring_.get_ring()));
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
    , event_loop_(ring_, file_registrar_, timeout_controller_, toUShort(ring_.get_ring()->sq.ring_entries)) {
    const auto queue_depth = ring_.get_ring()->sq.ring_entries;
    registered_iovecs_.resize(safe_add(toSize(queue_depth), 1uz).value_or(0uz));

    const auto probed = prober_.probe_io_paths(ring_.get_ring());
    write_path_       = probed.write_path;
    read_path_        = probed.read_path;
}

UringEngine::UringEngine(UringEngine&& other)
    : ring_(std::move(other.ring_))
    , file_registrar_(std::move(other.file_registrar_))
    , timeout_controller_(std::move(other.timeout_controller_))
    , event_loop_(ring_, file_registrar_, timeout_controller_, toUShort(ring_.get_ring()->sq.ring_entries))
    , read_buffers_registered_(other.read_buffers_registered_)
    , registered_iovecs_(std::move(other.registered_iovecs_))
    , buffer_register_error_(other.buffer_register_error_)
    , prober_(std::move(other.prober_))
    , write_path_(other.write_path_)
    , read_path_(other.read_path_) {
    other.read_buffers_registered_ = 0;
}

UringEngine& UringEngine::operator=(UringEngine&& other) {
    if (this != &other) {
        ring_               = std::move(other.ring_);
        file_registrar_     = std::move(other.file_registrar_);
        timeout_controller_ = std::move(other.timeout_controller_);

        std::destroy_at(&event_loop_);
        std::construct_at(
            &event_loop_, ring_, file_registrar_, timeout_controller_, toUShort(ring_.get_ring()->sq.ring_entries));

        read_buffers_registered_ = other.read_buffers_registered_;
        registered_iovecs_       = std::move(other.registered_iovecs_);
        buffer_register_error_   = other.buffer_register_error_;

        prober_     = std::move(other.prober_);
        write_path_ = other.write_path_;
        read_path_  = other.read_path_;

        other.read_buffers_registered_ = 0;
    }
    return *this;
}

UringEngine::~UringEngine() {
    if (ring_.get_ring()->ring_fd < 0) { return; }
    file_registrar_.unregister_file(ring_.get_ring());
    if (write_path_ == IoPath::Fixed || read_buffers_registered_ > 0) { io_uring_unregister_buffers(ring_.get_ring()); }
}

std::expected<void, UringError> UringEngine::register_buffers(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) noexcept {
    return BufferRegistrar { *this }.register_buffers(write_buf, read_bufs).transform_error([](std::error_code ec) {
        return UringError { ec };
    });
}

auto UringEngine::register_worker_affinity(const cpu_set_t* mask, std::size_t size) noexcept
    -> std::expected<void, UringError> {
    return posix::expect_success<posix::error_style::linux_internal>(
        io_uring_register_iowq_aff(ring_.get_ring(), size, mask))
        .transform_error(
            [](std::error_code ec) { return UringError { posix::SysCallError { ec, "io_uring_register_iowq_aff" } }; });
}

std::expected<PhaseRunStats, UringError> UringEngine::execute_write(const WriteContext& ctx) {
    event_loop_.set_paths(write_path_, read_path_, read_buffers_registered_);
    return event_loop_.execute(ctx);
}

std::expected<PhaseRunStats, UringError> UringEngine::execute_read(const ReadContext& ctx) {
    event_loop_.set_paths(write_path_, read_path_, read_buffers_registered_);
    return event_loop_.execute(ctx);
}

std::expected<void, std::error_code> BufferRegistrar::register_buffers(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) noexcept {

    if (!has_valid_buffer_registration_inputs(write_buf)) {
        fail_buffer_registration(posix::make_error(EINVAL));
        return std::unexpected(posix::make_error(EINVAL));
    }

    return compute_target_read_count(write_buf, read_bufs)
        .and_then([this, write_buf, read_bufs](std::size_t target_count) {
            populate_registered_iovecs(write_buf, read_bufs, target_count);
            return probe_adaptive_read_registration(target_count);
        })
        .transform([this](std::size_t read_count) {
            set_buffer_registration_state(read_count);
            engine_.write_path_ = IoPath::Fixed;
            engine_.read_path_  = IoPath::Fixed;
        })
        .transform_error([this](std::error_code error_code) {
            fail_buffer_registration(error_code);
            engine_.write_path_ = IoPath::Plain;
            engine_.read_path_  = IoPath::Plain;
            return error_code;
        });
}

std::size_t BufferRegistrar::max_registerable_iovecs() noexcept {
    constexpr std::size_t ct_limit = compile_time_iovec_limit();
    std::size_t limit              = ct_limit;
    bool has_limit                 = (ct_limit != std::numeric_limits<std::size_t>::max());

#ifdef _SC_IOV_MAX
    const auto runtime_limit_res     = posix::expect_result<posix::error_style::posix>(::sysconf(_SC_IOV_MAX));
    const std::int64_t runtime_limit = (runtime_limit_res && *runtime_limit_res > 0) ? *runtime_limit_res : -1L;
    if (runtime_limit > 0) {
        limit     = std::min(limit, toSize(runtime_limit));
        has_limit = true;
    }
#endif

    return has_limit ? limit : 1024uz;
}

bool BufferRegistrar::has_valid_buffer_registration_inputs(std::span<std::byte> write_buf) const noexcept {
    const auto queue_depth = engine_.ring_.get_ring()->sq.ring_entries;
    return !write_buf.empty() && engine_.registered_iovecs_.size() == safe_add(toSize(queue_depth), 1uz).value_or(0uz);
}

std::expected<std::size_t, std::error_code> BufferRegistrar::compute_memlock_read_limit(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) const noexcept {
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
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) const noexcept {
    const std::size_t max_iovecs = std::min(max_registerable_iovecs(), engine_.registered_iovecs_.size());
    if (max_iovecs == 0) { return std::unexpected(posix::make_error(E2BIG)); }

    return compute_memlock_read_limit(write_buf, read_bufs)
        .transform([max_iovecs, &read_bufs](std::size_t max_read_by_memlock) {
            const std::size_t max_read_by_iov = safe_sub(max_iovecs, 1uz).value_or(0uz);
            return std::ranges::min({ read_bufs.size(), max_read_by_iov, max_read_by_memlock });
        });
}

void BufferRegistrar::populate_registered_iovecs(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs, std::size_t read_count) noexcept {
    engine_.registered_iovecs_[0] = { .iov_base = write_buf.data(), .iov_len = write_buf.size() };

    const auto targets = engine_.registered_iovecs_ | std::views::drop(1) | std::views::take(read_count);
    std::ranges::for_each(std::views::zip(targets, read_bufs), [](auto&& pair) noexcept {
        auto&& [dest, src] = pair;
        dest               = { .iov_base = src.span().data(), .iov_len = src.span().size() };
    });
}

void BufferRegistrar::set_buffer_registration_state(std::size_t read_count) noexcept {
    engine_.read_buffers_registered_ = read_count;
    engine_.buffer_register_error_   = std::error_code {};
}

void BufferRegistrar::fail_buffer_registration(std::error_code error_code) noexcept {
    engine_.write_path_              = IoPath::Plain;
    engine_.read_path_               = IoPath::Plain;
    engine_.read_buffers_registered_ = 0;
    engine_.buffer_register_error_   = error_code;
}

std::expected<std::size_t, std::error_code> BufferRegistrar::probe_adaptive_read_registration(
    std::size_t target_read_count) noexcept {
    return probe_register_buffers(target_read_count, true)
        .transform([target_read_count]() { return target_read_count; })
        .or_else([this, target_read_count](std::error_code err) -> std::expected<std::size_t, std::error_code> {
            return (!is_buffer_register_resource_error(err) || target_read_count == 0)
                ? std::unexpected(err)
                : find_successful_probe_floor(target_read_count).and_then([this, target_read_count](std::size_t floor) {
                      return probe_max_read_count(floor, target_read_count);
                  });
        });
}

std::expected<void, std::error_code> BufferRegistrar::probe_register_buffers(
    std::size_t read_count, bool keep_registered) noexcept {
    const std::size_t iovec_count = safe_add(read_count, 1uz).value_or(0uz);

    return posix::expect_success<posix::error_style::linux_internal>(
        io_uring_register_buffers(engine_.ring_.get_ring(), engine_.registered_iovecs_.data(), toUInt(iovec_count)))
        .and_then([this, keep_registered]() -> std::expected<void, std::error_code> {
            return keep_registered ? std::expected<void, std::error_code> {}
                                   : posix::expect_success<posix::error_style::linux_internal>(
                                         io_uring_unregister_buffers(engine_.ring_.get_ring()));
        });
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

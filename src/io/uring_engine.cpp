/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "uring_engine.hpp"

namespace uring {

std::expected<void, std::error_code> UringRing::initialize_ring(
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

    std::int32_t last_ret = -1;
    const auto it
        = std::ranges::find_if(kFlagSets, [this, queue_depth, sq_thread_cpu, &last_ret](std::uint32_t flags) noexcept {
              struct io_uring_params params {};
              params.flags = flags;

              if (sq_thread_cpu.has_value() && (flags & IORING_SETUP_SQPOLL) != 0u) {
                  params.flags |= IORING_SETUP_SQ_AFF;
                  params.sq_thread_cpu = toUInt(*sq_thread_cpu);
              }

              last_ret = io_uring_queue_init_params(queue_depth, &ring_, &params);
              return last_ret == 0;
          });

    return (it != kFlagSets.end()) ? std::expected<void, std::error_code> {}
                                   : posix::expect_success<posix::error_style::linux_internal>(last_ret);
}

UringRing::UringRing(std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu) {
    const auto init_result = initialize_ring(queue_depth, sq_thread_cpu);

    init_       = init_result.has_value();
    init_error_ = init_result.error_or(std::error_code {});

    if (init_) {
        if (posix::expect_success<posix::error_style::linux_internal>(io_uring_register_ring_fd(&ring_))) {
            ring_fd_registered_ = true;
        }
    }
}

UringRing::~UringRing() {
    if (init_) {
        if (ring_fd_registered_) { io_uring_unregister_ring_fd(&ring_); }
        io_uring_queue_exit(&ring_);
    }
}

void UringProber::probe_io_paths(io_uring* ring, IoPath& write_path, IoPath& read_path) noexcept {
    struct io_uring_probe* probe = io_uring_get_probe_ring(ring);
    if (probe == nullptr) {
        ///< Probe unsupported (kernel < 5.6); default to Vector as a safe fallback
        ///< for pre-5.6 kernels (READV/WRITEV exist since 5.1).
        write_path = IoPath::Vector;
        read_path  = IoPath::Vector;
        return;
    }

    scope_exit free_probe { [probe]() noexcept { io_uring_free_probe(probe); } };

    if (io_uring_opcode_supported(probe, IORING_OP_WRITE) == 0) { write_path = IoPath::Vector; }
    if (io_uring_opcode_supported(probe, IORING_OP_READ) == 0) { read_path = IoPath::Vector; }
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

std::expected<void, std::string> UringTimeoutController::arm_timeout_timer(io_uring* ring) {
    timeout_ts_ = { .tv_sec = ::config::kDiskBenchmarkMaxSeconds, .tv_nsec = 0 };

    return posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(ring))
        .transform_error([](auto) { return std::string("Failed to get SQE for timer"); })
        .and_then([this, ring](io_uring_sqe* sqe) {
            io_uring_prep_timeout(sqe, &timeout_ts_, 0, 0);
            io_uring_sqe_set_data64(sqe, kTimerTag);

            return posix::expect_success<posix::error_style::linux_internal>(io_uring_submit(ring))
                .transform_error([](auto err) { return format_sys_error(err, "io_uring_submit (timer)"); });
        })
        .and_then([this]() -> std::expected<void, std::string> {
            timer_armed_ = true;
            return {};
        });
}

void UringTimeoutController::drain_pending_timer(io_uring* ring) noexcept {
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

    io_uring_cqe* cqe = nullptr;
    bool cancel_seen  = false;
    bool timer_seen   = false;
    __kernel_timespec ts {};
    ts.tv_sec  = 0;
    ts.tv_nsec = kDrainTimeoutNs;

    while (!(timer_seen && cancel_seen)) {
        if (!posix::expect_success<posix::error_style::linux_internal>(io_uring_wait_cqe_timeout(ring, &cqe, &ts))) {
            break;
        }

        const auto tag = io_uring_cqe_get_data64(cqe);
        const auto res = cqe->res; ///< Save before cqe_seen releases the slot.
        io_uring_cqe_seen(ring, cqe);
        if (tag == kTimerTag) { timer_seen = true; }
        if (tag != kCancelTag) { continue; }

        cancel_seen = true;

        /// Distinguish genuine cancel failures from benign race outcomes.
        const auto is_cancel_error
            = [](std::int32_t result) noexcept { return result < 0 && !is_one_of<-ENOENT, -EALREADY>(result); };
        if (is_cancel_error(res)) { print_warning(std::format("io_uring cancel failed: {}", res)); }
    }
}

std::expected<void, std::string> UringIoSubmitter::submit_batch(
    UringEngine& engine, const IoContext& ctx, bool is_write, UringEngine::LoopState& state) {
    const UringEngine::IoPrepContext prep { .io = ctx, .is_write = is_write };

    const auto retry_slots_view
        = std::span { engine.retry_slots_ }.subspan(0, engine.retry_count_) | std::views::reverse;
    std::size_t retries_submitted = 0;

    for (const auto idx : retry_slots_view) {
        const auto sqe_res
            = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(engine.ring_.get_ring()));
        if (!sqe_res) { break; }
        io_uring_sqe* sqe = *sqe_res;

        prepare_io_sqe(engine, sqe, prep, engine.requests_[idx], idx);
        retries_submitted += 1;
    }
    engine.retry_count_ -= retries_submitted;

    const std::size_t queue_in_flight  = state.submitted - state.completed;
    const std::size_t queue_limit      = ctx.queue_depth > queue_in_flight ? ctx.queue_depth - queue_in_flight : 0uz;
    const std::size_t blocks_remaining = ctx.total_blocks > state.submitted ? ctx.total_blocks - state.submitted : 0uz;
    const std::size_t limit            = std::min({ blocks_remaining, queue_limit, toSize(state.free_count) });

    const auto free_slots_view
        = state.free_slots.subspan(0, state.free_count) | std::views::reverse | std::views::take(limit);
    std::size_t submitted_in_batch = 0;

    for (const auto idx : free_slots_view) {
        if (state.interrupt || ctx.stop.stop_requested()) {
            state.interrupt  = true;
            state.free_count = toUShort(state.free_count - submitted_in_batch);
            return std::unexpected(std::string { config::kInterruptMsg });
        }

        const auto sqe_res
            = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(engine.ring_.get_ring()));
        if (!sqe_res) { break; }
        io_uring_sqe* sqe = *sqe_res;

        const std::uint64_t remaining_bytes = ctx.total_bytes - state.offset;
        const std::size_t len               = toSize(std::ranges::min(ctx.block_size, remaining_bytes));
        engine.requests_[idx]               = { state.offset, len, len, {} };

        prepare_io_sqe(engine, sqe, prep, engine.requests_[idx], idx);

        state.offset += len;
        submitted_in_batch += 1;
    }
    state.free_count = toUShort(state.free_count - submitted_in_batch);
    state.submitted += submitted_in_batch;

    return {};
}

IoPath UringIoSubmitter::determine_io_path(
    const UringEngine& engine, const UringEngine::IoPrepContext& prep, std::uint16_t idx) noexcept {
    if (prep.is_write) { return engine.write_path_; }

    if (engine.read_path_ != IoPath::Fixed) { return engine.read_path_; }

    return (toSize(idx) < engine.read_buffers_registered_) ? IoPath::Fixed : IoPath::Plain;
}

void UringIoSubmitter::prepare_io_sqe(UringEngine& engine, io_uring_sqe* sqe, const UringEngine::IoPrepContext& prep,
    IoRequest& req, std::uint16_t idx) noexcept {
    const std::size_t done    = req.total_len - req.remaining;
    const auto stride_mul     = toSize(idx) * prep.io.mem_stride;
    const auto subspan_offset = stride_mul + done;
    const auto slice          = prep.is_write ? prep.io.write_buffer.subspan(subspan_offset, req.remaining)
                                              : prep.io.read_buffers[idx].span().subspan(done, req.remaining);

    const unsigned len = toUInt(slice.size());
    /** @brief Use registered file index (0) if available, otherwise raw FD. */
    const posix::file_descriptor::native_handle_type fd
        = engine.file_registrar_.registered_handle() ? 0 : prep.io.fd.native_handle();

    const IoPath current_path = determine_io_path(engine, prep, idx);

    switch (current_path) {
        case IoPath::Fixed:
            if (prep.is_write) {
                io_uring_prep_write_fixed(sqe, fd, slice.data(), len, req.offset, 0);
            } else {
                io_uring_prep_read_fixed(sqe, fd, slice.data(), len, req.offset, idx + 1);
            }
            break;
        case IoPath::Vector:
            req.iov = { .iov_base = slice.data(), .iov_len = slice.size() };
            if (prep.is_write) {
                io_uring_prep_writev(sqe, fd, &req.iov, 1, req.offset);
            } else {
                io_uring_prep_readv(sqe, fd, &req.iov, 1, req.offset);
            }
            break;
        case IoPath::Plain:
            if (prep.is_write) {
                io_uring_prep_write(sqe, fd, slice.data(), len, req.offset);
            } else {
                io_uring_prep_read(sqe, fd, slice.data(), len, req.offset);
            }
            break;
    }

    req.start_cycles = tsc::rdtsc_ordered();
    io_uring_sqe_set_data64(sqe, idx);
    /** @brief Mark as fixed file if we are using the registered table index. */
    if (engine.file_registrar_.registered_handle()) { sqe->flags |= IOSQE_FIXED_FILE; }
}

std::expected<void, std::string> UringCqeProcessor::wait_for_submission(
    UringEngine& engine, const IoContext& ctx, std::uint32_t wait_nr) {
    io_uring_cqe* cqe_ptr = nullptr;
    auto& wait_ts         = engine.timeout_controller_.prepare_wait_timeout(config::kUringWaitTimeoutNs);

    return posix::expect_success<posix::error_style::linux_internal>(
        io_uring_submit_and_wait_timeout(engine.ring_.get_ring(), &cqe_ptr, wait_nr, &wait_ts, nullptr))
        .or_else([](std::error_code err) -> std::expected<void, std::error_code> {
            return is_retryable_wait_error(toInt(err.value())) ? std::expected<void, std::error_code> {}
                                                               : std::unexpected(err);
        })
        .transform_error([](auto err) { return format_sys_error(err, "io_uring_submit_and_wait"); })
        .and_then([&ctx]() -> std::expected<void, std::string> {
            return (ctx.interrupt_cb.get()() || ctx.stop.stop_requested())
                ? std::unexpected(std::string { config::kInterruptMsg })
                : std::expected<void, std::string> {};
        });
}

std::expected<void, std::string> UringCqeProcessor::process_completions(
    UringEngine& engine, const IoContext& ctx, bool is_write, UringEngine::LoopState& state) {
    const unsigned count = io_uring_peek_batch_cqe(
        engine.ring_.get_ring(), engine.cqe_buffer_.data(), toUInt(engine.cqe_buffer_.size()));
    if (count == 0) { return {}; }

    const auto cqe_span   = std::span { engine.cqe_buffer_ }.subspan(0, count);
    std::size_t processed = 0;

    scope_exit advance_cq { [&engine, &processed, &state]() noexcept {
        if (processed > 0) { io_uring_cq_advance(engine.ring_.get_ring(), toUInt(processed)); }
        std::ranges::for_each(state.deltas | std::views::take(state.delta_count),
            [&engine](const auto delta) noexcept { engine.hist_.add(delta); });
        state.delta_count = 0;
    } };

    for (const auto* cqe : cqe_span) {
        if (const auto res = handle_completion(engine, cqe, is_write, state); !res) {
            processed += 1;
            return res;
        }
        processed += 1;
    }

    ctx.progress_cb.get()(state.completed, ctx.total_blocks, ctx.label);
    return {};
}

std::expected<void, std::string> UringCqeProcessor::handle_completion(
    UringEngine& engine, const io_uring_cqe* cqe, bool is_write, UringEngine::LoopState& state) {
    const auto tag = io_uring_cqe_get_data64(cqe);
    if (tag == UringTimeoutController::kTimerTag) {
        engine.timeout_controller_.set_timer_armed(false);
        if (cqe->res == -ETIME) { return std::unexpected("Disk Benchmark Timeout"); }
        return {};
    }

    const auto idx_res = resolve_cqe_slot(engine, tag);
    if (!idx_res) { return std::unexpected(idx_res.error()); }
    const std::uint16_t idx = *idx_res;

    if (is_one_of<-EAGAIN, -EINTR>(cqe->res)) { return queue_retry_slot(engine, idx); }

    if (cqe->res == -EINVAL && (is_write ? engine.write_path_ : engine.read_path_) != IoPath::Vector) {
        if (is_write) {
            engine.write_path_ = IoPath::Vector;
        } else {
            engine.read_path_ = IoPath::Vector;
        }
        return queue_retry_slot(engine, idx);
    }

    return finalize_cqe(engine, cqe, is_write, idx, state);
}

std::expected<void, std::string> UringCqeProcessor::finalize_cqe(
    UringEngine& engine, const io_uring_cqe* cqe, bool is_write, std::uint16_t idx, UringEngine::LoopState& state) {
    const auto res_opt = posix::expect_result<posix::error_style::linux_internal>(cqe->res);
    if (!res_opt) { return std::unexpected(format_sys_error(res_opt.error(), is_write ? "write" : "read")); }
    const std::int32_t res = *res_opt;
    const auto bytes       = toSize(res);
    state.bytes_completed += bytes;

    if (bytes == 0) [[unlikely]] {
        return std::unexpected(
            is_write ? "Write operation stalled (0 bytes written)" : "Unexpected EOF (0 bytes read)");
    }

    if (bytes < engine.requests_[idx].remaining) {
        engine.requests_[idx].remaining -= bytes;
        engine.requests_[idx].offset += bytes;
        return queue_retry_slot(engine, idx);
    }

    ++state.completed;
    ++state.io_completed;

    const auto end_cycles = tsc::rdtscp_ordered();
    if (end_cycles > engine.requests_[idx].start_cycles) [[likely]] {
        state.deltas[state.delta_count] = end_cycles - engine.requests_[idx].start_cycles;
        ++state.delta_count;
    }

    state.free_slots[state.free_count] = idx;
    ++state.free_count;
    return {};
}

std::expected<void, std::string> UringCqeProcessor::queue_retry_slot(UringEngine& engine, std::uint16_t idx) noexcept {
    if (engine.retry_count_ >= engine.retry_slots_.size()) { return std::unexpected("Retry slot overflow"); }

    engine.retry_slots_[engine.retry_count_] = idx;
    ++engine.retry_count_;
    return {};
}

std::expected<std::uint16_t, std::string> UringCqeProcessor::resolve_cqe_slot(
    const UringEngine& engine, std::uint64_t tag) noexcept {
    const std::uint16_t idx = toUShort(tag);
    if (toSize(idx) >= engine.requests_.size()) [[unlikely]] { return std::unexpected("CQE tag out of bounds"); }

    return idx;
}

void UringCqeProcessor::drain_all_completions(
    UringEngine& engine, const IoContext& ctx, bool is_write, UringEngine::LoopState& state) noexcept {
    const auto drain_step = [&engine, &ctx, is_write, &state]() noexcept -> bool {
        const auto initial = state.completed;
        return wait_one_cqe(engine)
            .transform_error([](auto err) { return format_sys_error(err, "io_uring wait"); })
            .or_else([](auto) { return std::expected<void, std::string> {}; })
            .and_then([&engine, &ctx, is_write, &state]() -> std::expected<void, std::string> {
                return process_completions(engine, ctx, is_write, state);
            })
            .transform([&state, initial]() { return state.completed > initial || state.completed == state.submitted; })
            .value_or(false);
    };

    while (state.completed < state.submitted) {
        if (drain_step()) [[likely]] { continue; }
        print_warning(std::format("io_uring: drain stalled at {}/{} completions", state.completed, state.submitted));
        break;
    }
}

std::expected<void, std::error_code> UringCqeProcessor::wait_one_cqe(UringEngine& engine) noexcept {
    io_uring_cqe* cqe = nullptr;
    __kernel_timespec ts { .tv_sec = 0, .tv_nsec = config::kUringWaitTimeoutNs };
    return posix::expect_success<posix::error_style::linux_internal>(
        ::io_uring_wait_cqe_timeout(engine.ring_.get_ring(), &cqe, &ts));
}

UringEngine::UringEngine(std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu)
    : ring_(queue_depth, sq_thread_cpu) {
    if (ring_.is_valid()) {
        requests_.resize(queue_depth);
        retry_slots_.resize(queue_depth);
        free_slots_.resize(queue_depth);
        deltas_.resize(queue_depth);
        cqe_buffer_.resize(queue_depth);
        registered_iovecs_.resize(safe_add(toSize(queue_depth), 1uz).value_or(0uz));
        retry_count_ = 0;

        prober_.probe_io_paths(ring_.get_ring(), write_path_, read_path_);
    }
}

UringEngine::~UringEngine() {
    if (ring_.is_valid()) {
        file_registrar_.unregister_file(ring_.get_ring());
        if (write_path_ == IoPath::Fixed || read_buffers_registered_ > 0) {
            io_uring_unregister_buffers(ring_.get_ring());
        }
    }
}

std::expected<void, std::error_code> UringEngine::register_buffers(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) noexcept {
    return BufferRegistrar { *this }.register_buffers(write_buf, read_bufs);
}

std::expected<PhaseRunStats, std::string> UringEngine::submit_and_wait(const IoContext& ctx, bool is_write) {
    if (const auto res = file_registrar_.register_file(ring_.get_ring(), ctx.fd); !res) {
        if (res.error().value() == EBUSY) { return std::unexpected("io_uring: file registration conflict"); }
        print_warning("io_uring: fixed-file registration failed, using raw fd");
    }
    scope_exit unreg_file { [this]() noexcept { file_registrar_.unregister_file(ring_.get_ring()); } };

    LoopState state { .free_slots = free_slots_, .deltas = deltas_ };
    state.reset_free_slots(ctx.queue_depth);
    hist_ = {};

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
        if (timeout_controller_.is_timer_armed()) { timeout_controller_.drain_pending_timer(ring_.get_ring()); }
    } };

    /**
     * @brief Safety Drain: Ensure all in-flight I/O is reaped before destruction.
     *
     * If the benchmark is interrupted or fails early, we MUST wait for all
     * pending requests to complete (or cancel) before allowing the caller to
     * destroy the I/O buffers. Failure to do so would cause a Use-After-Free
     * when the kernel eventually writes to the (freed) buffer memory.
     */
    scope_exit drain_all { [this, &ctx, is_write, &state]() noexcept {
        if (state.completed < state.submitted) {
            UringCqeProcessor::drain_all_completions(*this, ctx, is_write, state);
        }
    } };

    /**
     * @brief Defines the fraction of the queue to wait for during batching.
     *
     * Sourced dynamically from Config::kIoBatchPercent. For example, a value of 25
     * means we wait for 25% of the queue to complete before waking up. This keeps
     * the storage controller saturated while reducing syscalls.
     */
    const std::uint32_t batch_size
        = std::max<std::uint32_t>(1, (toUInt(ctx.queue_depth) * ::config::kIoBatchPercent) / 100);

    while (state.completed < ctx.total_blocks) {
        if (const auto res = UringIoSubmitter::submit_batch(*this, ctx, is_write, state); !res) {
            return std::unexpected(res.error());
        }

        const auto active_requests_opt = safe_sub(state.submitted, state.completed);
        if (!active_requests_opt) [[unlikely]] {
            return std::unexpected("Logic Error: completed blocks exceed submitted blocks");
        }
        const std::size_t active_requests = *active_requests_opt;

        const auto in_kernel_opt = safe_sub(active_requests, retry_count_);
        if (!in_kernel_opt) [[unlikely]] { return std::unexpected("Logic Error: retry slots exceed active requests"); }
        const std::size_t in_kernel = *in_kernel_opt;

        if (active_requests == 0) { break; }

        /**
         * @brief Determines the batch wait threshold to balance throughput and system call overhead.
         *
         * Blocking wait is enforced when the queue capacity is exhausted or all blocks have been submitted,
         * avoiding CPU spinning while preserving device pipeline saturation.
         */
        const std::uint32_t wait_nr = determine_wait_count(in_kernel, batch_size, state, ctx);

        if (wait_nr > 0) {
            if (const auto wait = UringCqeProcessor::wait_for_submission(*this, ctx, wait_nr); !wait) {
                return std::unexpected(wait.error());
            }
        } else {
            io_uring_submit(ring_.get_ring());
        }

        const auto proc_res = UringCqeProcessor::process_completions(*this, ctx, is_write, state);
        if (!proc_res) { return std::unexpected(proc_res.error()); }
    }

    const auto t1 = std::chrono::steady_clock::now();
    return PhaseRunStats {
        .elapsed   = t1 - t0,
        .io_bytes  = state.bytes_completed,
        .total_ios = state.io_completed,
        .histogram = std::move(hist_),
    };
}

std::expected<void, std::error_code> BufferRegistrar::register_buffers(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) noexcept {
    std::error_code error_code = posix::make_error(EINVAL);
    scope_exit rollback { [this, &error_code]() noexcept { fail_buffer_registration(error_code); } };

    if (!has_valid_buffer_registration_inputs(write_buf)) { return std::unexpected(posix::make_error(EINVAL)); }

    const auto target_read_count = compute_target_read_count(write_buf, read_bufs);
    if (!target_read_count) {
        error_code = target_read_count.error();
        return std::unexpected(error_code);
    }

    populate_registered_iovecs(write_buf, read_bufs, *target_read_count);

    const auto read_count = probe_adaptive_read_registration(*target_read_count);

    if (read_count) {
        set_buffer_registration_state(*read_count);
        engine_.write_path_ = IoPath::Fixed;
        engine_.read_path_  = IoPath::Fixed;
    } else {
        engine_.write_path_ = IoPath::Plain;
        engine_.read_path_  = IoPath::Plain;
        error_code          = read_count.error();
        return std::unexpected(error_code);
    }

    error_code = std::error_code {};
    rollback.release();
    return {};
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
    return engine_.ring_.is_valid() && !write_buf.empty()
        && engine_.registered_iovecs_.size() == safe_add(engine_.requests_.size(), 1uz).value_or(0uz);
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

std::uint32_t UringEngine::determine_wait_count(
    std::size_t in_kernel, std::uint32_t batch_size, const LoopState& state, const IoContext& ctx) noexcept {
    if (state.free_count == 0 || state.submitted == ctx.total_blocks) {
        return std::min<std::uint32_t>(toUInt(in_kernel), batch_size);
    }
    return 0;
}

BufferRegistrar::MemlockBudget BufferRegistrar::pinned_memory_budget() const noexcept {
    const auto root_result = check_root_memlock_budget();

    return root_result
        .or_else([](auto) {
            return posix::get_rlimit(RLIMIT_MEMLOCK).transform([](const rlimit& limit) noexcept {
                return map_rlimit_to_budget(limit);
            });
        })
        .value_or(MemlockBudget { .state = MemlockBudgetState::Unknown, .bytes = 0 });
}

std::expected<BufferRegistrar::MemlockBudget, std::error_code>
BufferRegistrar::check_root_memlock_budget() const noexcept {
    if (::geteuid() == 0) { return MemlockBudget { .state = MemlockBudgetState::Unlimited, .bytes = 0 }; }
    return std::unexpected(posix::make_error(EPERM));
}

BufferRegistrar::MemlockBudget BufferRegistrar::map_rlimit_to_budget(const rlimit& limit) noexcept {
    if (limit.rlim_cur == RLIM_INFINITY) { return { .state = MemlockBudgetState::Unlimited, .bytes = 0 }; }
    return { .state = MemlockBudgetState::Limited, .bytes = toULong(limit.rlim_cur) };
}

} // namespace uring

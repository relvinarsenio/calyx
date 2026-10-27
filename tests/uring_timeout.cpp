/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/**
 * @file
 * @brief Exercises engine templates with synthetic queues and real liburing CQE handling.
 * @details Only submission and timed waits are wrapped, so these tests need no
 * io_uring syscalls. CTest bounds hangs caused by lost timer completions.
 */
#include "src/io/uring_engine.cpp"

#include <array>
#include <cstdio>
#include <print>
#include <string_view>

namespace {

struct SyntheticRing {
    std::array<io_uring_sqe, 8> sqes {};
    std::array<io_uring_cqe, 8> cqes {};
    unsigned sq_head {}, sq_tail {}, sq_flags {}, cq_head {}, cq_tail {};
    uring::UringRing owner {};

    explicit SyntheticRing(unsigned features) {
        auto* ring = owner.get_ring();
        ring->features = features;
        ring->sq.khead = &sq_head;
        ring->sq.ktail = &sq_tail;
        ring->sq.kflags = &sq_flags;
        ring->sq.sqes = sqes.data();
        ring->sq.ring_entries = static_cast<unsigned>(sqes.size());
        ring->sq.ring_mask = ring->sq.ring_entries - 1;
        ring->cq.khead = &cq_head;
        ring->cq.ktail = &cq_tail;
        ring->cq.cqes = cqes.data();
        ring->cq.ring_entries = static_cast<unsigned>(cqes.size());
        ring->cq.ring_mask = ring->cq.ring_entries - 1;
    }

    void complete(std::uint64_t tag, int result) {
        cqes.at(cq_tail++) = io_uring_cqe { .user_data = tag, .res = result, .flags = 0 };
    }
};

std::span<const int> wait_errors {};
std::size_t next_wait_error {};

bool check(bool condition, std::string_view message) {
    if (!condition) { std::println(stderr, "FAIL: {}", message); }
    return condition;
}

bool drain_cases(unsigned features) {
    constexpr auto outcomes = std::to_array<std::pair<int, int>>({
        { -ECANCELED, 0 }, { -ETIME, -ENOENT }, { -ETIME, -EALREADY },
    });
    constexpr std::array transient_errors { -EINTR, -EAGAIN, -ETIME };
    std::size_t cases {};

    for (const auto [timer_result, cancel_result] : outcomes) {
        for (const bool reversed : { false, true }) {
            for (const bool transient : { false, true }) {
                SyntheticRing fixture { features };
                auto* ring = fixture.owner.get_ring();
                uring::UringTimeoutController timer;
                const auto armed = timer.arm_timeout_timer(ring);
                if (!check(armed.has_value(), "arm timer")) { return false; }
                if (!check(fixture.sqes[0].user_data == uring::UringTimeoutController::kTimerTag,
                        "timer SQE uses the application tag")) { return false; }

                if ((features & IORING_FEAT_EXT_ARG) == 0) {
                    fixture.complete(LIBURING_UDATA_TIMEOUT, 0);
                    fixture.complete(LIBURING_UDATA_TIMEOUT, -ETIME);
                }
                const auto complete_timer = [&] {
                    fixture.complete(uring::UringTimeoutController::kTimerTag, timer_result);
                };
                const auto complete_cancel = [&] {
                    fixture.complete(uring::UringTimeoutController::kCancelTag, cancel_result);
                };
                if (reversed) {
                    complete_cancel();
                    complete_timer();
                } else {
                    complete_timer();
                    complete_cancel();
                }

                wait_errors = transient ? std::span<const int> { transient_errors } : std::span<const int> {};
                next_wait_error = 0;
                timer.drain_pending_timer(ring, std::chrono::milliseconds { 1 });
                if (!check(!timer.is_timer_armed() && fixture.cq_head == fixture.cq_tail,
                        "drain consumes both completions and disarms the timer")) { return false; }
                if (!check(fixture.sqes[1].opcode == IORING_OP_ASYNC_CANCEL
                            && fixture.sqes[1].addr == uring::UringTimeoutController::kTimerTag
                            && fixture.sqes[1].user_data == uring::UringTimeoutController::kCancelTag,
                        "cancellation targets the application timer")) { return false; }
                ++cases;
            }
        }
    }
    std::println("{} timer drain scenarios passed", cases);
    return true;
}

template <uring::IoContext Context> bool batch_case(unsigned features) {
    SyntheticRing fixture { features };
    uring::IoTracker tracker { 4 };
    uring::UringFileRegistrar registrar;
    uring::UringTimeoutController timer;
    uring::IoPathState paths;
    uring::CompletionQueue queue { { fixture.owner, registrar, timer, paths }, tracker, 4 };
    const posix::file_descriptor fd {};
    const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const> progress
        = [](std::size_t, std::size_t, std::string_view) {};
    const std::move_only_function<bool() const noexcept> interrupt = []() noexcept { return false; };
    const Context context { {
        .buffers = {},
        .fd = fd,
        .layout = uring::IoLayout::sequential(4096, 4096, 4096, 4),
        .observer = { .progress_cb = progress, .interrupt_cb = interrupt },
    } };

    timer.set_timer_armed(true);
    fixture.complete(LIBURING_UDATA_TIMEOUT, 0);
    fixture.complete(LIBURING_UDATA_TIMEOUT, -ETIME);
    const auto internal = queue.process_completions(context);
    if (!check(internal.has_value() && timer.is_timer_armed() && fixture.cq_head == fixture.cq_tail
                && tracker.state().completed == 0 && tracker.pending_count() == 0,
            "batch ignores internal timeouts without changing the application timer or requests")) { return false; }

    fixture.complete(uring::UringTimeoutController::kTimerTag, -ETIME);
    const auto expired = queue.process_completions(context);
    if (!check(!expired && std::get_if<uring::ExecutionError>(&expired.error())
                && std::get<uring::ExecutionError>(expired.error()) == uring::ExecutionError::Timeout
                && !timer.is_timer_armed(),
            "batch still reports the application deadline")) { return false; }

    fixture.complete(1024, 0);
    const auto invalid = queue.process_completions(context);
    return check(!invalid && std::get_if<uring::LogicError>(&invalid.error())
            && std::get<uring::LogicError>(invalid.error()) == uring::LogicError::CqeTagOutOfBounds,
        "batch still rejects invalid request tags");
}

} // namespace

extern "C" int __wrap_io_uring_submit(io_uring* ring) noexcept {
    const auto submitted = ring->sq.sqe_tail - ring->sq.sqe_head;
    ring->sq.sqe_head = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(submitted);
}

extern "C" int __wrap_io_uring_wait_cqe_timeout(io_uring* ring, io_uring_cqe** cqe, __kernel_timespec*) noexcept {
    if (next_wait_error < wait_errors.size()) {
        *cqe = nullptr;
        return wait_errors[next_wait_error++];
    }
    const int result = __io_uring_peek_cqe(ring, cqe, nullptr);
    return result != 0 ? result : (*cqe != nullptr ? 0 : -ETIME);
}

int main(int argc, char* argv[]) {
    if (argc != 2) { return 2; }
    const std::string_view mode { argv[1] };
    if (mode == "legacy") { return drain_cases(0) ? 0 : 1; }
    if (mode == "modern") { return drain_cases(IORING_FEAT_EXT_ARG) ? 0 : 1; }
    if (mode != "batch") { return 2; }
    for (const unsigned features : { 0u, static_cast<unsigned>(IORING_FEAT_EXT_ARG) }) {
        if (!batch_case<uring::WriteContext>(features) || !batch_case<uring::ReadContext>(features)) { return 1; }
    }
    std::println("4 batch completion scenarios passed");
    return 0;
}

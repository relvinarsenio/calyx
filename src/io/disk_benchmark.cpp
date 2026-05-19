/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "disk_benchmark.hpp"

#include "affinity.hpp"
#include "color.hpp"
#include "config.hpp"
#include "file_descriptor.hpp"
#include "latency_histogram.hpp"
#include "random_engine.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <print>
#include <random>
#include <ranges>
#include <span>
#include <sys/resource.h>
#include <sys/uio.h>
#include <tuple>
#include <unistd.h>
#include <vector>

#ifdef USE_IO_URING
#include <liburing.h>
#else
#error "Calyx requires Linux-native io_uring. Please install liburing or use the build-static.sh toolchain."
#endif

namespace {

using metrics::LatencyHistogram;
using prng::SplitMix64;
using prng::Xoshiro256PlusPlus;

/**
 * @brief Statistics snapshot for a benchmark phase execution.
 */
struct PhaseRunStats {
    std::chrono::duration<double> elapsed {};
    std::uint64_t io_bytes  = 0;
    std::uint64_t total_ios = 0;
    LatencyHistogram histogram;
};

/**
 * @brief Builds a metrics report from collected phase statistics.
 * @param stats The source statistics to process.
 */
[[nodiscard]] DiskIOMetrics make_disk_metrics(const PhaseRunStats& stats) {
    const auto runtime_sec = stats.elapsed.count();

    return DiskIOMetrics {
        .bw_bytes_per_sec = (runtime_sec > 0.0) ? (toDouble(stats.io_bytes) / runtime_sec) : 0.0,
        .cv               = stats.histogram.get_cv(),
    };
}

class IoFile {
    posix::file file_;

    explicit IoFile(posix::file&& file)
        : file_(std::move(file)) {}

public:
    IoFile(IoFile&&)            = default;
    IoFile& operator=(IoFile&&) = default;

    IoFile(const IoFile&)            = delete;
    IoFile& operator=(const IoFile&) = delete;

    [[nodiscard]] static std::expected<IoFile, std::error_code> create(
        const std::string& path, std::int32_t flags, mode_t mode) {
        return posix::file::open_direct(path, flags, mode).transform([](posix::file file) {
            return IoFile(std::move(file));
        });
    }

    [[nodiscard]] posix::file& file() { return file_; }
    [[nodiscard]] const posix::file_descriptor& descriptor() const { return file_.descriptor(); }
};

class AlignedBuffer {
    struct AlignedDeleter {
        std::size_t alignment {};
        void operator()(std::byte* ptr) const noexcept {
            if (ptr != nullptr) [[likely]] { ::operator delete(ptr, std::align_val_t { alignment }); }
        }
    };

    std::unique_ptr<std::byte[], AlignedDeleter> ptr_;
    std::size_t size_ = 0;

    explicit AlignedBuffer(std::byte* ptr, std::size_t size, std::size_t alignment) noexcept
        : ptr_(ptr, AlignedDeleter { alignment })
        , size_(size) {}

public:
    AlignedBuffer() noexcept
        : ptr_(nullptr, AlignedDeleter { 0 })
        , size_(0) {}

    AlignedBuffer(AlignedBuffer&& other) noexcept
        : ptr_(std::move(other.ptr_))
        , size_(std::exchange(other.size_, 0)) {}

    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) [[likely]] {
            ptr_  = std::move(other.ptr_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    AlignedBuffer(const AlignedBuffer&)            = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    [[nodiscard]] std::byte* data() noexcept { return ptr_.get(); }
    [[nodiscard]] const std::byte* data() const noexcept { return ptr_.get(); }

    [[nodiscard]] static std::optional<AlignedBuffer> create(std::size_t size, std::size_t alignment) noexcept {
        if (size == 0) [[unlikely]] { return std::nullopt; }
        if (!std::has_single_bit(alignment)) [[unlikely]] { return std::nullopt; }

        if (void* raw = ::operator new(size, std::align_val_t { alignment }, std::nothrow)) {
            auto* typed = static_cast<std::byte*>(raw);
            return AlignedBuffer(typed, size, alignment);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::span<std::byte> span() noexcept { return { ptr_.get(), size_ }; }

    [[nodiscard]] std::span<const std::byte> span() const noexcept { return { ptr_.get(), size_ }; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
};

/**
 * @brief Fills a buffer with a fast PRNG pattern (Xoshiro256++).
 * @details Uses a deterministic seed (digits of Pi) for benchmark
 * reproducibility. If high-entropy random payloads (e.g., for disk encryption
 * tests) are required, the state can be seeded from std::random_device.
 *
 * @param buffer The span of bytes to fill.
 */
void fill_pattern_fast(std::span<std::byte> buffer) noexcept {
    Xoshiro256PlusPlus rng { 0x243f6a8885a308d3, 0x6a09e667f3bcc908, 0x9e3779b97f4a7c15, 0x617078653320646e };

    constexpr auto word_size = sizeof(std::uint64_t);
    for (auto chunk : buffer | std::views::chunk(word_size)) {
        const auto value = rng();
        const auto bytes = std::bit_cast<std::array<std::byte, word_size>>(value);
        std::ranges::copy(bytes | std::views::take(chunk.size()), chunk.begin());
    }
}

#ifdef USE_IO_URING
/**
 * @brief I/O context containing descriptors, buffers and progress tracking.
 * @note  The `fd` reference must remain valid for the entire duration of the
 *        benchmark phase. This is safe here because `submit_and_wait` is
 *        synchronous, but would require ownership if the engine becomes async.
 */
struct IoContext {
    const posix::file_descriptor& fd;
    std::span<std::byte> write_buffer;
    std::span<AlignedBuffer> read_buffers;
    std::stop_token stop;
    std::reference_wrapper<
        const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const noexcept>>
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

/**
 * @brief Concept enforcing the contract for resource probing strategies.
 */
template <typename T>
concept ProberStrategy = requires(T& t, std::size_t count, bool keep) {
    { t.probe_register_buffers(count, keep) } -> std::same_as<std::expected<void, std::error_code>>;
};

class ResourceProber {
protected:
    [[nodiscard]] static bool is_buffer_register_resource_error(std::error_code ret) noexcept {
        return ret.value() == ENOMEM || ret.value() == E2BIG;
    }

    /**
     * @brief Performs a linear-step downward search to find a successful probe point.
     */
    template <typename Self>
        requires ProberStrategy<Self>
    [[nodiscard]] std::expected<std::size_t, std::error_code> find_successful_probe_floor(
        this Self& self, std::size_t high_fail) noexcept {
        std::size_t probe = high_fail;
        while (true) {
            probe >>= 1uz;
            auto res = self.probe_register_buffers(probe, false);
            if (res) { return probe; }
            if (!is_buffer_register_resource_error(res.error()) || probe == 0) { return std::unexpected(res.error()); }
        }
    }

    /**
     * @brief Performs a binary search between a known success and a known failure point.
     */
    template <typename Self>
        requires ProberStrategy<Self>
    [[nodiscard]] std::expected<std::size_t, std::error_code> probe_max_read_count(
        this Self& self, std::size_t low_success, std::size_t high_fail) noexcept {
        if (high_fail == 0) [[unlikely]] {
            return self.probe_register_buffers(low_success, true).transform([low_success]() { return low_success; });
        }

        std::size_t low  = low_success;
        std::size_t high = high_fail - 1uz;

        while (low < high) {
            const std::size_t mid = low + ((high - low + 1uz) >> 1uz);
            auto res              = self.probe_register_buffers(mid, false);

            if (res) {
                low = mid;
                continue;
            }
            if (is_buffer_register_resource_error(res.error())) {
                high = mid - 1uz;
                continue;
            }
            return std::unexpected(res.error());
        }

        return self.probe_register_buffers(low, true).transform([low]() { return low; });
    }
};

/**
 * @brief Asynchronous disk I/O engine backed by Linux io_uring.
 *
 * Wraps a single `io_uring` instance and exposes a high-level
 * submit-and-wait loop suitable for sequential disk benchmarks.
 * The engine is designed to be reused across write and read
 * phases within the same benchmark run — file registrations
 * are scoped per @ref submit_and_wait invocation.
 *
 * ## Optimisation layers (applied with graceful fallback)
 * | Priority | Feature                  | Notes                              |
 * |----------|--------------------------|------------------------------------|
 * | 1        | SQPOLL + COOP_TASKRUN    | Kernel clears SINGLE_ISSUER w/ SQP |
 * | 2        | SQPOLL only              | Older kernels reject COOP_TASKRUN  |
 * | 3        | COOP_TASKRUN             | Unprivileged / no SQPOLL           |
 * | 4        | Bare io_uring            | Any kernel with io_uring support   |
 *
 * On top of the ring flags, the engine also attempts:
 * - **Registered files** (`IOSQE_FIXED_FILE`) — eliminates per-I/O fd
 *   lookup overhead.
 * - **Registered buffers** (`io_uring_prep_{read,write}_fixed`) — pins
 *   memory once so the kernel skips repeated page-table walks.
 * - **Vector I/O fallback** (`writev`/`readv`) for kernels < 5.6 that
 *   do not support `IORING_OP_WRITE`/`IORING_OP_READ`.
 */
class BufferRegistrar;

class UringEngine {
    enum class IoPath : std::uint8_t {
        Fixed,
        Plain,
        Vector
    };
    friend class BufferRegistrar;
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

    static constexpr std::uint64_t kTimerTag      = UINT64_MAX; ///< user_data sentinel for the timeout SQE.
    static constexpr std::uint64_t kCancelTag     = UINT64_MAX - 1; ///< user_data sentinel for the cancel SQE.
    static constexpr std::int64_t kDrainTimeoutNs = 134'217'728LL; ///< ~134 ms drain guard.
    io_uring ring_ {};
    bool init_ = false;
    std::error_code init_error_ {};
    bool ring_fd_registered_ = false;
    /** @brief Tracking handle for the currently registered file (fixed-file mode). */
    std::optional<posix::file_descriptor::native_handle_type> registered_handle_ = std::nullopt;
    bool timer_armed_ = false; ///< Tracks whether a timeout SQE is pending.
    __kernel_timespec timeout_ts_ {};
    __kernel_timespec wait_ts_ {};
    std::error_code buffer_register_error_ {};

    LatencyHistogram hist_ {};

    IoPath write_path_                   = IoPath::Plain;
    IoPath read_path_                    = IoPath::Plain;
    std::size_t read_buffers_registered_ = 0;

    std::vector<IoRequest> requests_;
    std::vector<std::uint16_t> retry_slots_ {};
    std::vector<std::uint16_t> free_slots_ {};
    std::vector<std::uint64_t> deltas_ {};
    std::vector<iovec> registered_iovecs_ {};
    std::size_t retry_count_ = 0;

    struct IoPrepContext {
        const IoContext& io;
        bool is_write = false;
    };

public:
    /**
     * @brief Construct a UringEngine with the given queue depth.
     *
     * @param queue_depth   Maximum number of in-flight I/O requests.
     * @param sq_thread_cpu Optional CPU core for SQPOLL thread pinning.
     */
    explicit UringEngine(std::uint16_t queue_depth, std::optional<std::int32_t> sq_thread_cpu = std::nullopt) {
        [[assume(queue_depth > 0)]];

        const auto init_result = [this, queue_depth, sq_thread_cpu]() -> std::expected<void, std::error_code> {
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
            for (const std::uint32_t flags : kFlagSets) {
                struct io_uring_params params {};
                params.flags = flags;

                if (sq_thread_cpu.has_value() && (flags & IORING_SETUP_SQPOLL) != 0u) {
                    params.flags |= IORING_SETUP_SQ_AFF;
                    params.sq_thread_cpu = toUInt(*sq_thread_cpu);
                }

                last_ret = io_uring_queue_init_params(queue_depth, &ring_, &params);
                if (last_ret == 0) { return {}; }
            }
            return posix::expect_success<posix::error_style::linux_internal>(last_ret);
        }();

        init_       = init_result.has_value();
        init_error_ = init_result.error_or(std::error_code {});

        if (init_) {
            requests_.resize(queue_depth);
            retry_slots_.resize(queue_depth);
            free_slots_.resize(queue_depth);
            deltas_.resize(queue_depth);
            registered_iovecs_.resize(toSize(queue_depth) + 1uz);
            retry_count_ = 0;

            if (posix::expect_success<posix::error_style::linux_internal>(io_uring_register_ring_fd(&ring_))) {
                ring_fd_registered_ = true;
            }
            probe_io_paths();
        }
    }

    UringEngine(const UringEngine&)            = delete;
    UringEngine& operator=(const UringEngine&) = delete;
    UringEngine(UringEngine&&)                 = delete;
    UringEngine& operator=(UringEngine&&)      = delete;

    ~UringEngine() {
        if (init_) {
            unregister_file();
            if (ring_fd_registered_) { io_uring_unregister_ring_fd(&ring_); }
            if (write_path_ == IoPath::Fixed || read_buffers_registered_ > 0) { io_uring_unregister_buffers(&ring_); }
            io_uring_queue_exit(&ring_);
        }
    }

    [[nodiscard]] bool is_valid() const noexcept { return init_; }
    [[nodiscard]] std::error_code get_error() const noexcept { return init_error_; }
    [[nodiscard]] std::error_code get_buffer_register_error() const noexcept { return buffer_register_error_; }

    [[nodiscard]] auto register_worker_affinity(const cpu_set_t* mask, std::size_t size) noexcept
        -> std::expected<void, std::error_code> {
        if (!init_) { return std::unexpected(posix::make_error(EINVAL)); }
        return posix::expect_success<posix::error_style::linux_internal>(
            ::io_uring_register_iowq_aff(&ring_, size, mask));
    }

    /**
     * @brief Register I/O buffers with the kernel for fixed-buffer operations.
     *
     * Layout of the registered iovec array:
     * | Index | Content                               |
     * |-------|---------------------------------------|
     * | 0     | Write buffer (single, reused per I/O) |
     * | 1..N  | Read buffers (one per queue slot)     |
     *
     * Registration is adaptive under user limits: the engine attempts to
     * register as many read buffers as possible (plus the write buffer),
     * then uses an exponential-down probe followed by binary search to
     * converge on the maximum accepted count.
     *
     * @param write_buf  Span of the aligned write buffer.
     * @param read_bufs  Span of per-slot aligned read buffers.
     * @return `true` if at least the write buffer is registered.
     */
    [[nodiscard]] std::expected<void, std::error_code> register_buffers(
        std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) noexcept;

    /**
     * @brief Submit all I/O blocks and wait for completion.
     *
     * Arms a timeout SQE, then enters a completion-driven loop that
     * submits work in batches of up to `queue_depth` and processes
     * CQEs as they arrive.  The timeout and file registration are
     * both cleaned up via `scope_exit` so the ring is always left
     * in a clean state — this is essential because the same engine
     * is reused for the write phase followed by the read phase.
     *
     * @param ctx       I/O context (fd, buffers, progress, etc.).
     * @param is_write  `true` for write benchmark, `false` for read.
     * @return          `std::expected<void>` — error string on failure.
     */
    [[nodiscard]] std::expected<PhaseRunStats, std::string> submit_and_wait(const IoContext& ctx, bool is_write) {
        if (auto res = register_file(ctx.fd); !res) {
            if (res.error().value() == EBUSY) { return std::unexpected("io_uring: file registration conflict"); }
            print_warning("io_uring: fixed-file registration failed, using raw fd");
        }
        scope_exit unreg_file { [this]() noexcept { unregister_file(); } };

        LoopState state { .free_slots = free_slots_, .deltas = deltas_ };
        state.reset_free_slots(ctx.queue_depth);
        hist_ = {};

        if (auto arm = arm_timeout_timer(); !arm) { return std::unexpected(arm.error()); }

        auto t0 = std::chrono::steady_clock::now();

        /**
         * @brief Always drain pending timer SQE, including early-return paths.
         *
         * This is required because the same UringEngine instance is reused
         * for the subsequent read phase; a stale timer CQE would corrupt the
         * next completion-processing pass.
         */
        scope_exit drain_timer { [this]() noexcept {
            if (timer_armed_) { drain_pending_timer(); }
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
            if (state.completed < state.submitted) { drain_all_completions(ctx, is_write, state); }
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
            if (auto res = submit_batch(ctx, is_write, state); !res) { return std::unexpected(res.error()); }

            const std::size_t active_requests = state.submitted - state.completed;
            const std::size_t in_kernel       = active_requests - retry_count_;
            if (active_requests == 0) { break; }

            /**
             * @brief Calculate how many completions to wait for (wait_nr).
             *
             * If the queue is full (free_count == 0) or we have submitted all blocks,
             * we must block and wait for a batch of completions. Otherwise, we just
             * flush the submission queue without blocking.
             */
            const std::uint32_t wait_nr = [in_kernel, batch_size, &state, &ctx]() -> std::uint32_t {
                if (state.free_count == 0 || state.submitted == ctx.total_blocks) {
                    return std::min<std::uint32_t>(toUInt(in_kernel), batch_size);
                }
                return 0;
            }();

            if (wait_nr > 0) {
                if (auto wait = wait_for_submission(ctx, wait_nr); !wait) { return std::unexpected(wait.error()); }
            } else {
                io_uring_submit(&ring_);
            }

            auto proc_res = process_completions(ctx, is_write, state);
            if (!proc_res) { return std::unexpected(proc_res.error()); }
        }

        auto t1 = std::chrono::steady_clock::now();
        return PhaseRunStats {
            .elapsed   = t1 - t0,
            .io_bytes  = state.bytes_completed,
            .total_ios = state.io_completed,
            .histogram = std::move(hist_),
        };
    }

private:
    /**
     * @brief Register a single file descriptor for fixed-file operations.
     *
     * Once registered the engine passes index 0 (instead of the raw fd)
     * into all prep functions and sets `IOSQE_FIXED_FILE`.
     *
     * @param fd_wrapper  The file descriptor wrapper to register.
     * @return `true` on success.
     */
    [[nodiscard]] auto register_file(const posix::file_descriptor& fd_wrapper) noexcept
        -> std::expected<void, std::error_code> {
        if (!init_) { return std::unexpected(posix::make_error(EINVAL)); }

        const auto fd = fd_wrapper.native_handle();
        if (registered_handle_) {
            if (*registered_handle_ == fd) { return {}; }
            return std::unexpected(posix::make_error(EBUSY));
        }

        return posix::expect_success<posix::error_style::linux_internal>(io_uring_register_files(&ring_, &fd, 1))
            .and_then([this, fd]() -> std::expected<void, std::error_code> {
                registered_handle_ = fd;
                return {};
            });
    }

    /** @brief Unregister the previously registered file, if any. */
    void unregister_file() noexcept {
        if (!registered_handle_) { return; }
        io_uring_unregister_files(&ring_);
        registered_handle_ = std::nullopt;
    }

    /**
     * @brief Probe kernel for supported I/O opcodes via IORING_REGISTER_PROBE.
     *
     * Detects whether non-vectored IORING_OP_READ/WRITE are supported (kernel >= 5.6).
     * Sets the default I/O path to Vector if they are not, avoiding the cost of
     * a wasted first-IO EINVAL from the CQE-driven fallback path.
     *
     * @see liburing io_uring_get_probe_ring(3), io_uring_opcode_supported(3).
     */
    void probe_io_paths() noexcept {
        struct io_uring_probe* probe = io_uring_get_probe_ring(&ring_);
        if (probe == nullptr) {
            ///< Probe unsupported (kernel < 5.6); default to Vector as a safe fallback
            ///< for pre-5.6 kernels (READV/WRITEV exist since 5.1).
            write_path_ = IoPath::Vector;
            read_path_  = IoPath::Vector;
            return;
        }

        scope_exit free_probe { [probe]() noexcept { io_uring_free_probe(probe); } };

        if (io_uring_opcode_supported(probe, IORING_OP_WRITE) == 0) { write_path_ = IoPath::Vector; }
        if (io_uring_opcode_supported(probe, IORING_OP_READ) == 0) { read_path_ = IoPath::Vector; }
    }

    [[nodiscard]] auto wait_one_cqe() noexcept -> std::expected<void, std::error_code> {
        io_uring_cqe* cqe = nullptr;
        __kernel_timespec ts { .tv_sec = 0, .tv_nsec = config::kUringWaitTimeoutNs };
        return posix::expect_success<posix::error_style::linux_internal>(
            ::io_uring_wait_cqe_timeout(&ring_, &cqe, &ts));
    }

    void drain_all_completions(const IoContext& ctx, bool is_write, LoopState& state) noexcept {
        const auto drain_step = [this, &ctx, is_write, &state]() noexcept -> bool {
            const auto initial = state.completed;
            return wait_one_cqe()
                .transform_error([](auto err) { return format_sys_error(err, "io_uring wait"); })
                .or_else([](auto) { return std::expected<void, std::string> {}; })
                .and_then([this, &ctx, is_write, &state]() { return process_completions(ctx, is_write, state); })
                .transform(
                    [&state, initial]() { return state.completed > initial || state.completed == state.submitted; })
                .value_or(false);
        };

        while (state.completed < state.submitted) {
            if (drain_step()) [[likely]] { continue; }
            print_warning(
                std::format("io_uring: drain stalled at {}/{} completions", state.completed, state.submitted));
            break;
        }
    }

    /**
     * @brief Fill the SQ with pending retries and new work up to queue_depth.
     *
     * @return Success or error string.
     */
    [[nodiscard]] auto submit_batch(const IoContext& ctx, bool is_write, LoopState& state)
        -> std::expected<void, std::string> {
        const IoPrepContext prep { .io = ctx, .is_write = is_write };

        while (retry_count_ > 0) {
            auto sqe_res = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(&ring_));
            if (!sqe_res) { return {}; }
            io_uring_sqe* sqe = *sqe_res;

            auto idx = retry_slots_[--retry_count_];

            prepare_io_sqe(sqe, prep, requests_[idx], idx);
        }

        while (state.submitted < ctx.total_blocks && (state.submitted - state.completed) < ctx.queue_depth) {
            [[assume(ctx.total_blocks > 0uz)]];
            if (state.interrupt || ctx.stop.stop_requested()) {
                state.interrupt = true;
                return std::unexpected(std::string { config::kInterruptMsg });
            }

            if (state.free_count == 0) { break; }

            auto sqe_res = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(&ring_));
            if (!sqe_res) { break; }
            io_uring_sqe* sqe = *sqe_res;

            auto idx = state.free_slots[--state.free_count];

            const std::uint64_t remaining_bytes = ctx.total_bytes - state.offset;
            const std::size_t len               = toSize(std::ranges::min(ctx.block_size, remaining_bytes));
            requests_[idx]                      = { state.offset, len, len, {} };

            prepare_io_sqe(sqe, prep, requests_[idx], idx);

            state.offset += len;
            state.submitted++;
        }
        return {};
    }

    [[nodiscard]] static bool is_retryable_wait_error(std::int32_t rc) noexcept {
        return rc == ETIME || rc == EINTR || rc == EAGAIN || rc == EBUSY;
    }

    [[nodiscard]] std::expected<void, std::string> arm_timeout_timer() {
        timeout_ts_ = { .tv_sec = ::config::kDiskBenchmarkMaxSeconds, .tv_nsec = 0 };

        return posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(&ring_))
            .transform_error([](auto) { return std::string("Failed to get SQE for timer"); })
            .and_then([this](io_uring_sqe* sqe) {
                io_uring_prep_timeout(sqe, &timeout_ts_, 0, 0);
                io_uring_sqe_set_data64(sqe, kTimerTag);

                return posix::expect_success<posix::error_style::linux_internal>(io_uring_submit(&ring_))
                    .transform_error([](auto err) { return format_sys_error(err, "io_uring_submit (timer)"); });
            })
            .and_then([this]() -> std::expected<void, std::string> {
                timer_armed_ = true;
                return {};
            });
    }

    [[nodiscard]] std::expected<void, std::string> wait_for_submission(const IoContext& ctx, std::uint32_t wait_nr) {
        io_uring_cqe* cqe_ptr = nullptr;
        wait_ts_              = { .tv_sec = 0, .tv_nsec = config::kUringWaitTimeoutNs };

        return posix::expect_success<posix::error_style::linux_internal>(
            io_uring_submit_and_wait_timeout(&ring_, &cqe_ptr, wait_nr, &wait_ts_, nullptr))
            .or_else([](std::error_code err) -> std::expected<void, std::error_code> {
                if (is_retryable_wait_error(toInt(err.value()))) { return {}; }
                return std::unexpected(err);
            })
            .transform_error([](auto err) { return format_sys_error(err, "io_uring_submit_and_wait"); })
            .and_then([&ctx]() -> std::expected<void, std::string> {
                if (ctx.interrupt_cb.get()() || ctx.stop.stop_requested()) [[unlikely]] {
                    return std::unexpected(std::string { config::kInterruptMsg });
                }
                return {};
            });
    }

    /**
     * @brief Cancel and consume the pending timeout SQE.
     *
     * Submits an `IORING_OP_ASYNC_CANCEL` targeting @ref kTimerTag,
     * then waits (with a short guard timeout) until both the cancel CQE
     * and the targeted timer CQE are seen. This leaves the ring free
     * of stale CQEs so the engine can be safely reused for a subsequent I/O phase.
     */
    void drain_pending_timer() noexcept {
        if (!timer_armed_) { return; }
        scope_exit reset_timer { [this]() noexcept { timer_armed_ = false; } };

        auto sqe_res
            = posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(&ring_)).or_else([this](auto) {
                  io_uring_submit(&ring_);
                  return posix::expect_result<posix::error_style::pointer>(io_uring_get_sqe(&ring_));
              });

        if (!sqe_res) { return; }
        io_uring_sqe* sqe = *sqe_res;

        io_uring_prep_cancel64(sqe, kTimerTag, 0);
        io_uring_sqe_set_data64(sqe, kCancelTag);

        if (!posix::expect_success<posix::error_style::linux_internal>(io_uring_submit(&ring_))) { return; }

        io_uring_cqe* cqe = nullptr;
        bool cancel_seen  = false;
        bool timer_seen   = false;
        __kernel_timespec ts {};
        ts.tv_sec  = 0;
        ts.tv_nsec = kDrainTimeoutNs;

        while (!(timer_seen && cancel_seen)) {
            if (!posix::expect_success<posix::error_style::linux_internal>(
                    io_uring_wait_cqe_timeout(&ring_, &cqe, &ts))) {
                break;
            }

            const auto tag = io_uring_cqe_get_data64(cqe);
            const auto res = cqe->res; ///< Save before cqe_seen releases the slot.
            io_uring_cqe_seen(&ring_, cqe);
            if (tag == kTimerTag) { timer_seen = true; }
            if (tag != kCancelTag) { continue; }

            cancel_seen = true;

            /// Distinguish genuine cancel failures from benign race outcomes.
            const auto is_cancel_error
                = [](std::int32_t result) noexcept { return result < 0 && result != -ENOENT && result != -EALREADY; };
            if (is_cancel_error(res)) { print_warning(std::format("io_uring cancel failed: {}", res)); }
        }
    }

    [[nodiscard]] std::expected<void, std::string> queue_retry_slot(std::uint16_t idx) noexcept {
        if (retry_count_ >= retry_slots_.size()) { return std::unexpected("Retry slot overflow"); }

        retry_slots_[retry_count_++] = idx;
        return {};
    }

    [[nodiscard]] std::expected<std::uint16_t, std::string> resolve_cqe_slot(std::uint64_t tag) const noexcept {
        const std::uint16_t idx = toUShort(tag);
        if (toSize(idx) >= requests_.size()) [[unlikely]] { return std::unexpected("CQE tag out of bounds"); }

        return idx;
    }

    [[nodiscard]] std::expected<void, std::string> finalize_cqe(
        const io_uring_cqe* cqe, bool is_write, std::uint16_t idx, LoopState& state) {
        return posix::expect_result<posix::error_style::linux_internal>(cqe->res)
            .transform_error(
                [is_write](std::error_code err) { return format_sys_error(err, is_write ? "write" : "read"); })
            .and_then([this, &state, idx, is_write](std::int32_t res) -> std::expected<void, std::string> {
                const auto bytes = toSize(res);
                state.bytes_completed += bytes;

                if (bytes == 0) [[unlikely]] {
                    return std::unexpected(
                        is_write ? "Write operation stalled (0 bytes written)" : "Unexpected EOF (0 bytes read)");
                }

                if (bytes < requests_[idx].remaining) {
                    requests_[idx].remaining -= bytes;
                    requests_[idx].offset += bytes;
                    return queue_retry_slot(idx);
                }

                ++state.completed;
                ++state.io_completed;

                const auto end_cycles = tsc::rdtscp_ordered();
                if (end_cycles > requests_[idx].start_cycles) [[likely]] {
                    state.deltas[state.delta_count++] = end_cycles - requests_[idx].start_cycles;
                }

                state.free_slots[state.free_count++] = idx;
                return {};
            });
    }

    /**
     * @brief Fill the SQ with operation details using the optimal I/O path.
     *
     * Resolves the correct IoPath (Fixed, Vector, or Plain) based on the current
     * engine state and slot index, then prepares the corresponding SQE command.
     */
    void prepare_io_sqe(io_uring_sqe* sqe, const IoPrepContext& prep, IoRequest& req, std::uint16_t idx) noexcept {
        const std::size_t done = req.total_len - req.remaining;
        auto slice             = prep.is_write
                        ? prep.io.write_buffer.subspan(toSize(idx) * prep.io.mem_stride + done, req.remaining)
                        : prep.io.read_buffers[idx].span().subspan(done, req.remaining);

        const unsigned len = toUInt(slice.size());
        /** @brief Use registered file index (0) if available, otherwise raw FD. */
        const posix::file_descriptor::native_handle_type fd = registered_handle_ ? 0 : prep.io.fd.native_handle();

        const IoPath current_path = [this, prep, idx]() noexcept -> IoPath {
            if (prep.is_write) { return write_path_; }

            if (read_path_ != IoPath::Fixed) { return read_path_; }

            return (toSize(idx) < read_buffers_registered_) ? IoPath::Fixed : IoPath::Plain;
        }();

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
        if (registered_handle_) { sqe->flags |= IOSQE_FIXED_FILE; }
    }

    /**
     * @brief Process a single completion entry (CQE).
     *
     * Handles timeout sentinels, transient system errors (retries),
     * and dynamic path fallback (e.g., to Vector I/O on EINVAL).
     */
    [[nodiscard]] std::expected<void, std::string> handle_completion(
        const io_uring_cqe* cqe, bool is_write, LoopState& state) {
        auto tag = io_uring_cqe_get_data64(cqe);
        if (tag == kTimerTag) {
            timer_armed_ = false;
            if (cqe->res == -ETIME) { return std::unexpected("Disk Benchmark Timeout"); }
            return {};
        }

        auto idx_res = resolve_cqe_slot(tag);
        if (!idx_res) { return std::unexpected(idx_res.error()); }
        const std::uint16_t idx = *idx_res;

        if (cqe->res == -EAGAIN || cqe->res == -EINTR) { return queue_retry_slot(idx); }

        if (cqe->res == -EINVAL && (is_write ? write_path_ : read_path_) != IoPath::Vector) {
            if (is_write) {
                write_path_ = IoPath::Vector;
            } else {
                read_path_ = IoPath::Vector;
            }
            return queue_retry_slot(idx);
        }

        return finalize_cqe(cqe, is_write, idx, state);
    }

    /**
     * @brief Harvest all available CQEs from the ring and update benchmark state.
     *
     * Iterates through the CQ ring, delegating logic to @ref handle_completion
     * and ensuring the ring head is advanced atomically after processing.
     */
    [[nodiscard]] std::expected<void, std::string> process_completions(
        const IoContext& ctx, bool is_write, LoopState& state) {
        std::uint32_t head  = 0;
        std::uint16_t count = 0;
        io_uring_cqe* cqe   = nullptr;

        scope_exit advance_cq { [this, &count, &state]() noexcept {
            if (count > 0) { io_uring_cq_advance(&ring_, count); }
            for (const auto delta : state.deltas | std::views::take(state.delta_count)) {
                hist_.add(delta);
            }
            state.delta_count = 0;
        } };

        io_uring_for_each_cqe(&ring_, head, cqe) {
            count++;
            if (auto res = handle_completion(cqe, is_write, state); !res) { return res; }
        }

        if (count > 0) { ctx.progress_cb.get()(state.completed, ctx.total_blocks, ctx.label); }

        return {};
    }
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

[[nodiscard]] std::expected<void, std::error_code> UringEngine::register_buffers(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) noexcept {
    return BufferRegistrar { *this }.register_buffers(write_buf, read_bufs);
}

[[nodiscard]] std::expected<void, std::error_code> BufferRegistrar::register_buffers(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) noexcept {
    std::error_code error_code = posix::make_error(EINVAL);
    scope_exit rollback { [this, &error_code]() noexcept { fail_buffer_registration(error_code); } };

    if (!has_valid_buffer_registration_inputs(write_buf)) { return std::unexpected(posix::make_error(EINVAL)); }

    auto target_read_count = compute_target_read_count(write_buf, read_bufs);
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

[[nodiscard]] std::size_t BufferRegistrar::max_registerable_iovecs() noexcept {
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    bool has_limit    = false;

#ifdef UIO_MAXIOV
    limit     = std::min(limit, toSize(UIO_MAXIOV));
    has_limit = true;
#endif

#ifdef IOV_MAX
    if (IOV_MAX > 0) {
        limit     = std::min(limit, toSize(IOV_MAX));
        has_limit = true;
    }
#endif

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

[[nodiscard]] bool BufferRegistrar::has_valid_buffer_registration_inputs(
    std::span<std::byte> write_buf) const noexcept {
    return engine_.init_ && !write_buf.empty() && engine_.registered_iovecs_.size() == engine_.requests_.size() + 1uz;
}

[[nodiscard]] std::expected<std::size_t, std::error_code> BufferRegistrar::compute_memlock_read_limit(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) const noexcept {
    const auto memlock_budget = pinned_memory_budget();
    if (memlock_budget.state != MemlockBudgetState::Limited) { return read_bufs.size(); }

    const auto write_size = write_buf.size();
    if (memlock_budget.bytes < write_size) { return std::unexpected(posix::make_error(ENOMEM)); }

    auto remaining                  = memlock_budget.bytes - write_size;
    std::size_t max_read_by_memlock = 0;
    for (const auto& buffer : read_bufs) {
        const auto buffer_size = buffer.size();
        if (remaining < buffer_size) { break; }

        remaining -= buffer_size;
        ++max_read_by_memlock;
    }

    return max_read_by_memlock;
}

[[nodiscard]] std::expected<std::size_t, std::error_code> BufferRegistrar::compute_target_read_count(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs) const noexcept {
    const std::size_t max_iovecs = std::min(max_registerable_iovecs(), engine_.registered_iovecs_.size());
    if (max_iovecs == 0) { return std::unexpected(posix::make_error(E2BIG)); }

    return compute_memlock_read_limit(write_buf, read_bufs)
        .transform([max_iovecs, &read_bufs](std::size_t max_read_by_memlock) {
            const std::size_t max_read_by_iov = max_iovecs - 1uz;
            return std::ranges::min({ read_bufs.size(), max_read_by_iov, max_read_by_memlock });
        });
}

void BufferRegistrar::populate_registered_iovecs(
    std::span<std::byte> write_buf, std::span<AlignedBuffer> read_bufs, std::size_t read_count) noexcept {
    engine_.registered_iovecs_[0] = { .iov_base = write_buf.data(), .iov_len = write_buf.size() };

    auto targets = engine_.registered_iovecs_ | std::views::drop(1) | std::views::take(read_count);
    for (auto&& [dest, src] : std::views::zip(targets, read_bufs)) {
        dest = { .iov_base = src.span().data(), .iov_len = src.span().size() };
    }
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

[[nodiscard]] std::expected<std::size_t, std::error_code> BufferRegistrar::probe_adaptive_read_registration(
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

[[nodiscard]] std::expected<void, std::error_code> BufferRegistrar::probe_register_buffers(
    std::size_t read_count, bool keep_registered) noexcept {
    const std::size_t iovec_count = read_count + 1uz;

    return posix::expect_success<posix::error_style::linux_internal>(
        io_uring_register_buffers(&engine_.ring_, engine_.registered_iovecs_.data(), toUInt(iovec_count)))
        .and_then([this, keep_registered]() -> std::expected<void, std::error_code> {
            if (keep_registered) { return {}; }
            return posix::expect_success<posix::error_style::linux_internal>(
                io_uring_unregister_buffers(&engine_.ring_));
        });
}

[[nodiscard]] BufferRegistrar::MemlockBudget BufferRegistrar::pinned_memory_budget() const noexcept {
    const auto root_result = []() -> std::expected<MemlockBudget, std::error_code> {
        if (::geteuid() == 0) { return MemlockBudget { .state = MemlockBudgetState::Unlimited, .bytes = 0 }; }
        return std::unexpected(posix::make_error(EPERM));
    }();

    const auto limit_mapper = [](const rlimit& limit) -> MemlockBudget {
        if (limit.rlim_cur == RLIM_INFINITY) { return { .state = MemlockBudgetState::Unlimited, .bytes = 0 }; }
        return { .state = MemlockBudgetState::Limited, .bytes = toULong(limit.rlim_cur) };
    };

    return root_result
        .or_else([&limit_mapper](auto) { return posix::get_rlimit(RLIMIT_MEMLOCK).transform(limit_mapper); })
        .value_or(MemlockBudget { .state = MemlockBudgetState::Unknown, .bytes = 0 });
}

#endif

/**
 * @brief Calculates the common alignment mask required for both read and write block sizes.
 *
 * @note This ensures that the total file size remains a valid multiple for both sequential
 * write and read passes, preventing unaligned or partial block I/O at EOF.
 */
[[nodiscard]] std::expected<std::size_t, std::string> calculate_final_mask(
    std::uint64_t write_block_size, std::uint64_t read_block_size) noexcept {
    if (write_block_size == 0 || read_block_size == 0) { return 0; }
    const std::size_t gcd_val = std::gcd(write_block_size, read_block_size);
    std::size_t lcm_val       = write_block_size / gcd_val;
    if (__builtin_mul_overflow(lcm_val, read_block_size, &lcm_val)) {
        return std::unexpected("Invalid configuration: block size combination results in alignment overflow");
    }
    return lcm_val;
}

/**
 * @brief Validates that the target directory has sufficient physical storage capacity.
 *
 * @note This prevents mid-benchmark crashes or corrupted test files caused by running
 * out of disk space during high-throughput I/O generation.
 */
[[nodiscard]] std::expected<std::filesystem::path, std::string> perform_space_check(
    std::uint64_t total_bytes, const std::filesystem::path& dir_path) {
    std::error_code ec;
    const auto target_path = dir_path.empty() ? std::filesystem::current_path(ec) : dir_path;
    if (ec) { return std::unexpected(format_sys_error(ec, "path resolution")); }

    if (auto res = check_disk_space(target_path, total_bytes); !res) {
        return std::unexpected(format_sys_error(res.error(), "Storage"));
    }
    return target_path;
}

struct Buffers {
    AlignedBuffer write;
    std::vector<AlignedBuffer> read;
};

/**
 * @brief Shared computed I/O parameters passed to each benchmark phase.
 *
 * @note Groups the post-alignment values that both write and read phases need
 * to construct their respective IoContext. Eliminates the need for a generic
 * make_ctx lambda that branches on is_write, keeping each phase self-contained.
 */
struct IoParams {
    std::span<std::byte> write_buffer;
    std::span<AlignedBuffer> read_buffers;
    std::stop_token stop;
    std::reference_wrapper<
        const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const noexcept>>
        progress_cb;
    std::reference_wrapper<const std::move_only_function<bool() const noexcept>> interrupt_cb;
    std::uint64_t total_bytes;
    std::uint64_t write_block_size;
    std::uint64_t read_block_size;
    std::uint64_t write_mem_stride;
    std::uint64_t read_mem_stride;
    std::string_view label;
    std::uint16_t write_queue_depth;
    std::uint16_t read_queue_depth;
};

/**
 * @brief Pre-allocates and aligns all required I/O memory buffers for the benchmark.
 *
 * @note Utilizing page-aligned or hardware-aligned buffers is mandatory for O_DIRECT
 * kernel interactions to bypass the page cache successfully.
 */
[[nodiscard]] std::expected<Buffers, std::string> allocate_io_buffers(std::uint64_t write_mem_stride,
    std::size_t alignment, const DiskBenchmark::BenchmarkConfig& config, std::uint64_t read_block_size,
    const auto& round_up) {
    std::size_t write_buf_total = 0;
    if (__builtin_mul_overflow(write_mem_stride, toSize(config.write_queue_depth), &write_buf_total)) {
        return std::unexpected("Overflow in write buffer total size calculation");
    }

    const auto write_buf_alloc_opt = round_up(write_buf_total, get_page_size());
    if (!write_buf_alloc_opt) { return std::unexpected("Overflow in write buffer allocation alignment"); }
    const auto write_buf_alloc = *write_buf_alloc_opt;
    auto write_buf_opt         = AlignedBuffer::create(write_buf_alloc, alignment);
    if (!write_buf_opt) { return std::unexpected("OOM: Failed to allocate aligned write buffer"); }
    AlignedBuffer write_buf_local = std::move(*write_buf_opt);
    fill_pattern_fast(write_buf_local.span());

    std::vector<AlignedBuffer> read_buffers_pool;
    read_buffers_pool.reserve(config.read_queue_depth);
    const auto read_buf_alloc_opt = round_up(read_block_size, get_page_size());
    if (!read_buf_alloc_opt) { return std::unexpected("Overflow in read buffer allocation alignment"); }
    const auto read_buf_alloc = *read_buf_alloc_opt;

    for (auto _ : std::views::iota(0uz, toSize(config.read_queue_depth))) {
        auto read_buf_opt = AlignedBuffer::create(read_buf_alloc, alignment);
        if (!read_buf_opt) { return std::unexpected("OOM: Failed to allocate read partitions"); }
        read_buffers_pool.push_back(std::move(*read_buf_opt));
    }
    return Buffers { std::move(write_buf_local), std::move(read_buffers_pool) };
}

/**
 * @brief Locks the io_uring engine and its kernel worker threads to a specific CPU core.
 *
 * @note Enforcing strict CPU affinity prevents context-switching latency and maximizes
 * L1/L2 cache locality, which is critical for saturating modern NVMe devices.
 */
[[nodiscard]] std::expected<void, std::string> setup_engine_affinity(
    std::optional<UringEngine>& engine, std::uint16_t max_queue_depth) {
    const auto affinity_res = affinity::execute_strict_isolation(
        [&engine, max_queue_depth](const cpu_set_t* mask, std::size_t size, std::uint32_t,
            std::int32_t target_cpu) noexcept -> std::expected<void, affinity::IsolationError> {
            /**
             * @note Engine creation inside the affinity callback is intentional:
             * the stabilized target_cpu is required for IORING_SETUP_SQ_AFF,
             * which explicitly hard-pins the SQPOLL kernel thread to that core.
             * SQPOLL threads do NOT inherit process CPU affinity.
             */
            engine.emplace(max_queue_depth, target_cpu);
            if (!engine->is_valid()) {
                return std::unexpected(
                    affinity::IsolationError { .ec = engine->get_error(), .context = "io_uring_queue_init failed" });
            }

            /** @brief Register async IO worker affinity (io-wq). */
            if (auto res = engine->register_worker_affinity(mask, size); !res) {
                return std::unexpected(
                    affinity::IsolationError { .ec = res.error(), .context = "io_uring_register_iowq_aff failed" });
            }
            return {};
        });

    if (!affinity_res) {
        const auto& err = affinity_res.error();
        return std::unexpected(format_sys_error(err.ec, err.context));
    }

    return {};
}

/**
 * @brief Executes the sequential write benchmark phase.
 *
 * @note Opens the test file exclusively, pre-allocates disk space, submits all write I/O
 * through io_uring, then flushes data to persistent storage. Partial files are cleaned up
 * on failure via scope_exit to prevent stale test artifacts.
 */
[[nodiscard]] std::expected<PhaseRunStats, std::string> execute_write_phase(
    const std::string& filename, UringEngine& engine, const IoParams& params) {
    std::error_code pre_ec;
    std::filesystem::remove(filename, pre_ec);

    bool write_completed = false;
    scope_exit remove_partial_file { [&filename, &write_completed]() noexcept {
        if (write_completed) { return; }

        std::error_code remove_ec;
        std::filesystem::remove(filename, remove_ec);
    } };

    const auto phase_result
        = IoFile::create(filename, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR)
              .transform_error([](std::error_code ec) { return format_sys_error(ec, "File Creation"); })
              .and_then([&engine, &params](IoFile wf) -> std::expected<PhaseRunStats, std::string> {
                  if (auto alloc = wf.file().allocate(0, toLong(params.total_bytes));
                      !alloc && alloc.error().value() != EOPNOTSUPP && alloc.error().value() != EINVAL) {
                      return std::unexpected(format_sys_error(alloc.error(), "posix_fallocate"));
                  }

                  std::string label_write = std::format("{} Write", params.label);
                  const auto ctx          = IoContext {
                               .fd           = wf.descriptor(),
                               .write_buffer = params.write_buffer,
                               .read_buffers = {},
                               .stop         = params.stop,
                               .progress_cb  = params.progress_cb,
                               .interrupt_cb = params.interrupt_cb,
                               .total_blocks = (params.total_bytes + params.write_block_size - 1) / params.write_block_size,
                               .block_size   = params.write_block_size,
                               .mem_stride   = params.write_mem_stride,
                               .total_bytes  = params.total_bytes,
                               .label        = label_write,
                               .queue_depth  = params.write_queue_depth,
                  };

                  return engine.submit_and_wait(ctx, true)
                      .and_then([&wf](const PhaseRunStats& io_result) {
                          return wf.file()
                              .datasync()
                              .transform_error([](std::error_code ec) { return format_sys_error(ec, "fdatasync"); })
                              .transform([io_result]() { return io_result; });
                      })
                      .transform([&wf](const PhaseRunStats& done) {
                          if (auto adv = wf.file().advise(0, 0, posix::FAdvise::DontNeed); !adv) {
                              print_warning(format_sys_error(adv.error(), "posix_fadvise"));
                          }
                          return done;
                      });
              });

    if (phase_result) { write_completed = true; }

    return phase_result;
}

/**
 * @brief Executes the sequential read benchmark phase.
 *
 * @note Opens the previously written test file in read-only mode and submits
 * all read I/O through io_uring to measure sustained read throughput.
 */
[[nodiscard]] std::expected<PhaseRunStats, std::string> execute_read_phase(
    const std::string& filename, UringEngine& engine, const IoParams& params) {
    return IoFile::create(filename, O_RDONLY, 0)
        .transform_error([](std::error_code ec) { return format_sys_error(ec, "File Open Read"); })
        .and_then([&engine, &params](IoFile rf) -> std::expected<PhaseRunStats, std::string> {
            std::string label_read = std::format("{} Read", params.label);
            const auto ctx         = IoContext {
                        .fd           = rf.descriptor(),
                        .write_buffer = {},
                        .read_buffers = params.read_buffers,
                        .stop         = params.stop,
                        .progress_cb  = params.progress_cb,
                        .interrupt_cb = params.interrupt_cb,
                        .total_blocks = (params.total_bytes + params.read_block_size - 1) / params.read_block_size,
                        .block_size   = params.read_block_size,
                        .mem_stride   = params.read_mem_stride,
                        .total_bytes  = params.total_bytes,
                        .label        = label_read,
                        .queue_depth  = params.read_queue_depth,
            };

            return engine.submit_and_wait(ctx, false).transform([](const PhaseRunStats& io_result) {
                return io_result;
            });
        });
}

} // namespace

std::expected<DiskIORunResult, std::string> DiskBenchmark::run_io_test(const BenchmarkConfig& config,
    const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const noexcept>& progress_cb,
    std::stop_token stop, const std::move_only_function<bool() const noexcept>& interrupt_cb) {
    using namespace std::chrono;

    if (config.write_queue_depth == 0 || config.read_queue_depth == 0) {
        return std::unexpected("Invalid configuration: queue depth must be positive");
    }

    if (config.write_block_size == 0 || config.read_block_size == 0) {
        return std::unexpected("Invalid configuration: block size must be positive");
    }

    if (!std::has_single_bit(config.alignment)) {
        return std::unexpected("Invalid configuration: alignment must be a power of two");
    }

    const std::string filename = get_test_filename();
    const auto dir_path        = std::filesystem::path(filename).parent_path();
    const posix::BlockSize hw  = posix::get_block_size(dir_path.empty() ? "." : dir_path);

    /**
     * @brief Auto-adjust I/O parameters to ensure O_DIRECT compatibility (man 2 open, man 2 statx).
     *
     * 1. alignment: Must be at least stx_dio_mem_align for memory buffer addresses.
     * 2. block_size: Must be a multiple of stx_dio_offset_align for I/O lengths and offsets.
     * 3. total_bytes: Must be a multiple of write_block_size to avoid partial-block EOF writes.
     */
    const auto alignment = std::max({ config.alignment, toSize(hw.mem_align), toSize(get_page_size()) });

    /** @brief [Rule of Clarity] Helper for rounding up values to a given alignment with overflow protection. */
    const auto round_up = [](std::uint64_t val, std::uint64_t align) noexcept -> std::optional<std::uint64_t> {
        if (align == 0) { return val; }
        const std::uint64_t rem = val % align;
        if (rem == 0) { return val; }
        const std::uint64_t diff = align - rem;
        if (val > std::numeric_limits<std::uint64_t>::max() - diff) { return std::nullopt; }
        return val + diff;
    };

    const auto write_block_size_opt = round_up(config.write_block_size, hw.offset_align);
    if (!write_block_size_opt) { return std::unexpected("Overflow in write block size alignment"); }
    const std::uint64_t write_block_size = *write_block_size_opt;

    const auto read_block_size_opt = round_up(config.read_block_size, hw.offset_align);
    if (!read_block_size_opt) { return std::unexpected("Overflow in read block size alignment"); }
    const std::uint64_t read_block_size = *read_block_size_opt;

    /**
     * @brief Adaptive Smart Alignment:
     * - If block_size is large (>= Page), align stride to Page for maximum kernel efficiency.
     * - If block_size is small (< Page), align stride to Hardware limit to avoid memory waste.
     */
    const auto write_mem_stride_opt = (write_block_size >= get_page_size())
        ? round_up(write_block_size, get_page_size())
        : round_up(write_block_size, hw.mem_align);
    if (!write_mem_stride_opt) { return std::unexpected("Overflow in write memory stride alignment"); }
    const auto write_mem_stride = *write_mem_stride_opt;

    const auto read_mem_stride_opt = (read_block_size >= get_page_size()) ? round_up(read_block_size, get_page_size())
                                                                          : round_up(read_block_size, hw.mem_align);
    if (!read_mem_stride_opt) { return std::unexpected("Overflow in read memory stride alignment"); }
    const auto read_mem_stride = *read_mem_stride_opt;

    const auto raw_size = toULong(config.size_mb) * 1024ULL * 1024ULL;

    /**
     * @brief Ensure total file size is a common multiple of both block sizes
     * for clean sequential passes in both write and read phases.
     */
    auto final_mask_res = calculate_final_mask(write_block_size, read_block_size);
    if (!final_mask_res) { return std::unexpected(final_mask_res.error()); }
    const std::size_t final_mask = *final_mask_res;

    const auto total_bytes_opt = round_up(raw_size, final_mask);
    if (!total_bytes_opt) { return std::unexpected("Overflow in total test size alignment"); }
    const std::uint64_t total_bytes = *total_bytes_opt;

    scope_exit file_cleaner { [&filename]() noexcept {
        std::error_code ec;
        std::filesystem::remove(filename, ec);
    } };

    auto space_check_res = perform_space_check(total_bytes, dir_path);
    if (!space_check_res) { return std::unexpected(space_check_res.error()); }

    auto buffers_res = allocate_io_buffers(write_mem_stride, alignment, config, read_block_size, round_up);
    if (!buffers_res) { return std::unexpected(buffers_res.error()); }
    auto&& [write_buf, read_buffers] = std::move(*buffers_res);

    const std::uint16_t max_queue_depth = std::max(config.write_queue_depth, config.read_queue_depth);
    std::optional<UringEngine> engine;

    /** @brief Enforce strict single-core isolation with explicit SQPOLL and io-wq pinning. */
    if (auto res = setup_engine_affinity(engine, max_queue_depth); !res) { return std::unexpected(res.error()); }

    static std::atomic<bool> warned { false };
    if (auto res = engine->register_buffers(write_buf.span(), read_buffers); !res && !warned.exchange(true)) {
        print_warning(std::format("Performance Hint: io_uring fixed buffers disabled ({}), using fallback.",
            res.error().value() == ENOMEM ? "Memory limit" : "System restriction"));
    }

    const IoParams params {
        .write_buffer      = write_buf.span(),
        .read_buffers      = read_buffers,
        .stop              = stop,
        .progress_cb       = std::cref(progress_cb),
        .interrupt_cb      = std::cref(interrupt_cb),
        .total_bytes       = total_bytes,
        .write_block_size  = write_block_size,
        .read_block_size   = read_block_size,
        .write_mem_stride  = write_mem_stride,
        .read_mem_stride   = read_mem_stride,
        .label             = config.label,
        .write_queue_depth = config.write_queue_depth,
        .read_queue_depth  = config.read_queue_depth,
    };

    return execute_write_phase(filename, *engine, params)
        .and_then([&filename, &engine, &params, &config](const PhaseRunStats& write_stats) {
            return execute_read_phase(filename, *engine, params)
                .transform([write_stats, &config](const PhaseRunStats& read_stats) {
                    const auto write_metrics = make_disk_metrics(write_stats);
                    const auto read_metrics  = make_disk_metrics(read_stats);

                    return DiskIORunResult {
                        .label = config.label,
                        .write = write_metrics,
                        .read  = read_metrics,
                    };
                });
        });
}

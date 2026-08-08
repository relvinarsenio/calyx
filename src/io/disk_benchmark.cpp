/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "disk_benchmark.hpp"

#include "affinity.hpp"
#include "aligned_buffer.hpp"
#include "color.hpp"
#include "config.hpp"
#include "file_descriptor.hpp"
#include "random_engine.hpp"
#include "tsc.hpp"
#include "uring_engine.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <numeric>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using prng::SplitMix64;
using prng::Xoshiro256PlusPlus;

/**
 * @brief Builds a metrics report from collected phase statistics.
 * @param stats The source statistics to process.
 */

[[nodiscard]] DiskIOMetrics make_disk_metrics(const PhaseRunStats& stats) {
    const auto runtime_sec = stats.elapsed.count();
    const metrics::LatencyAnalyzer analyzer { stats.histogram, DiskBenchmark::get_timer_calibration_ns() };

    return DiskIOMetrics {
        .histogram        = stats.histogram,
        .bw_bytes_per_sec = (runtime_sec > 0.0) ? (toDouble(stats.io_bytes) / runtime_sec) : 0.0,
        .cv               = analyzer.cv(),
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
    std::ranges::for_each(buffer | std::views::chunk(word_size), [&rng](auto chunk) noexcept {
        const auto value = rng();
        const auto bytes = std::bit_cast<std::array<std::byte, word_size>>(value);
        std::ranges::copy(bytes | std::views::take(chunk.size()), chunk.begin());
    });
}

/**
 * @brief Calculates the common alignment mask required for both read and write block sizes.
 *
 * @note This ensures that the total file size remains a valid multiple for both sequential
 * write and read passes, preventing unaligned or partial block I/O at EOF.
 */
[[nodiscard]] std::expected<std::size_t, BenchmarkError> calculate_final_mask(
    std::uint64_t write_block_size, std::uint64_t read_block_size) noexcept {
    if (write_block_size == 0 || read_block_size == 0) { return 0; }
    const std::uint64_t gcd_val     = std::gcd(write_block_size, read_block_size);
    const std::uint64_t lcm_partial = write_block_size / gcd_val;
    const auto lcm_val              = safe_mul(lcm_partial, read_block_size);
    if (!lcm_val) {
        return std::unexpected(BenchmarkError {
            BenchmarkError::Phase::Configuration, uring::UringError { uring::ConfigError::AlignmentOverflow } });
    }
    return *lcm_val;
}

/**
 * @brief Validates that the target directory has sufficient physical storage capacity.
 *
 * @note This prevents mid-benchmark crashes or corrupted test files caused by running
 * out of disk space during high-throughput I/O generation.
 */
[[nodiscard]] std::expected<std::filesystem::path, BenchmarkError> perform_space_check(
    std::uint64_t total_bytes, const std::filesystem::path& dir_path) {
    std::error_code ec {};
    const auto target_path = dir_path.empty() ? std::filesystem::current_path(ec) : dir_path;
    if (ec) {
        return std::unexpected(
            BenchmarkError { BenchmarkError::Phase::SpaceCheck, posix::SysCallError { ec, "path resolution" } });
    }

    if (const auto res = check_disk_space(target_path, total_bytes); !res) {
        return std::unexpected(
            BenchmarkError { BenchmarkError::Phase::SpaceCheck, posix::SysCallError { res.error(), "Storage" } });
    }
    return target_path;
}

struct Buffers {
    memory::AlignedBuffer write {};
    std::vector<memory::AlignedBuffer> read {};
};

/**
 * @brief Shared computed I/O parameters passed to each benchmark phase.
 *
 * @note Groups the post-alignment values that both write and read phases need
 * to construct their respective IoContext. Eliminates the need for a generic
 * make_ctx lambda that branches on is_write, keeping each phase self-contained.
 */
struct IoParams {
    std::span<std::byte> write_buffer {};
    std::span<memory::AlignedBuffer> read_buffers {};
    std::stop_token stop {};
    std::reference_wrapper<const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const>>
        progress_cb;
    std::reference_wrapper<const std::move_only_function<bool() const noexcept>> interrupt_cb;
    std::uint64_t total_bytes       = 0;
    std::uint64_t write_block_size  = 0;
    std::uint64_t read_block_size   = 0;
    std::uint64_t write_mem_stride  = 0;
    std::uint64_t read_mem_stride   = 0;
    std::string_view label          = "";
    std::uint16_t write_queue_depth = 0;
    std::uint16_t read_queue_depth  = 0;
};

/** @brief Helper for rounding up values to a given alignment with overflow protection. */
[[nodiscard]] constexpr std::optional<std::uint64_t> round_up(std::uint64_t val, std::uint64_t align) noexcept {
    if (align == 0) { return val; }
    const std::uint64_t rem = val % align;
    if (rem == 0) { return val; }
    const std::uint64_t diff = safe_sub(align, rem).value_or(0ULL);
    return safe_add(val, diff);
}

/**
 * @brief Pre-allocates and aligns all required I/O memory buffers for the benchmark.
 *
 * @note Utilizing page-aligned or hardware-aligned buffers is mandatory for O_DIRECT
 * kernel interactions to bypass the page cache successfully.
 */
[[nodiscard]] std::expected<Buffers, BenchmarkError> allocate_io_buffers(std::uint64_t write_mem_stride,
    std::size_t alignment, const DiskBenchmark::BenchmarkConfig& config, std::uint64_t read_block_size) {
    const auto write_buf_total = safe_mul(write_mem_stride, config.write_queue_depth);
    if (!write_buf_total) {
        return std::unexpected(BenchmarkError { BenchmarkError::Phase::BufferAllocation,
            uring::UringError { uring::AllocationError::WriteBufSizeOverflow } });
    }

    const auto write_buf_alloc_opt = round_up(*write_buf_total, get_page_size());
    if (!write_buf_alloc_opt) {
        return std::unexpected(BenchmarkError { BenchmarkError::Phase::BufferAllocation,
            uring::UringError { uring::AllocationError::WriteBufAlignOverflow } });
    }
    const auto write_buf_alloc = *write_buf_alloc_opt;
    auto write_buf_opt         = memory::AlignedBuffer::create(write_buf_alloc, alignment);
    if (!write_buf_opt) {
        return std::unexpected(BenchmarkError {
            BenchmarkError::Phase::BufferAllocation, uring::UringError { uring::AllocationError::WriteBufOom } });
    }
    memory::AlignedBuffer write_buf_local = std::move(*write_buf_opt);
    fill_pattern_fast(write_buf_local);

    const auto read_buf_alloc_opt = round_up(read_block_size, get_page_size());
    if (!read_buf_alloc_opt) {
        return std::unexpected(BenchmarkError { BenchmarkError::Phase::BufferAllocation,
            uring::UringError { uring::AllocationError::ReadBufAlignOverflow } });
    }
    const auto read_buf_alloc = *read_buf_alloc_opt;

    const auto init_vector = [depth = config.read_queue_depth]() {
        std::vector<memory::AlignedBuffer> vec;
        vec.reserve(depth);
        return vec;
    };

    auto read_buffers_res = std::ranges::fold_left(std::views::iota(0uz, toSize(config.read_queue_depth)),
        std::expected<std::vector<memory::AlignedBuffer>, BenchmarkError> { init_vector() },
        [read_buf_alloc, alignment](
            auto&& acc, auto) -> std::expected<std::vector<memory::AlignedBuffer>, BenchmarkError> {
            if (!acc) { return std::move(acc); }
            auto read_buf_opt = memory::AlignedBuffer::create(read_buf_alloc, alignment);
            if (!read_buf_opt) {
                return std::unexpected(BenchmarkError { BenchmarkError::Phase::BufferAllocation,
                    uring::UringError { uring::AllocationError::ReadBufOom } });
            }
            acc->push_back(std::move(*read_buf_opt));
            return std::move(acc);
        });

    if (!read_buffers_res) { return std::unexpected(read_buffers_res.error()); }
    return Buffers { std::move(write_buf_local), std::move(*read_buffers_res) };
}

/**
 * @brief Submits write I/O through io_uring after pre-allocating disk space.
 *
 * @note Assumes the file is already opened exclusively. Flushes data to persistent
 * storage and drops page-cache pages after completion.
 */
[[nodiscard]] std::expected<PhaseRunStats, BenchmarkError> run_write_operations(
    IoFile wf, UringEngine& engine, const IoParams& params) {
    if (const auto alloc = wf.file().allocate(0, toLong(params.total_bytes));
        !alloc && alloc.error().value() != EOPNOTSUPP && alloc.error().value() != EINVAL) {
        return std::unexpected(BenchmarkError {
            BenchmarkError::Phase::WritePhase, posix::SysCallError { alloc.error(), "posix_fallocate" } });
    }

    const std::string label_write = std::format("{} Write", params.label);
    const auto ctx                = uring::WriteContext { {
                       .buffers      = params.write_buffer,
                       .fd           = wf.descriptor(),
                       .layout       = {
                           .total_blocks = params.total_bytes / params.write_block_size,
                           .block_size   = params.write_block_size,
                           .mem_stride   = params.write_mem_stride,
                           .total_bytes  = params.total_bytes,
                           .queue_depth  = params.write_queue_depth,
                       },
                       .observer     = {
                           .label        = label_write,
                           .stop         = params.stop,
                           .progress_cb  = params.progress_cb,
                           .interrupt_cb = params.interrupt_cb,
                       }
    } };

    return engine.execute_write(ctx)
        .transform_error(
            [](const uring::UringError& err) { return BenchmarkError { BenchmarkError::Phase::WritePhase, err }; })
        .and_then([&wf](const PhaseRunStats& io_result) -> std::expected<PhaseRunStats, BenchmarkError> {
            return wf.file()
                .datasync()
                .transform_error([](std::error_code ec) {
                    return BenchmarkError { BenchmarkError::Phase::WritePhase,
                        posix::SysCallError { ec, "fdatasync" } };
                })
                .transform([io_result]() { return io_result; });
        })
        .transform([&wf](const PhaseRunStats& done) {
            if (const auto adv = wf.file().advise(0, 0, posix::FAdvise::DontNeed); !adv) {
                print_warning(format_sys_error(adv.error(), "posix_fadvise"));
            }
            return done;
        });
}

/**
 * @brief Executes the sequential write benchmark phase.
 *
 * @note Opens the test file exclusively, pre-allocates disk space, submits all write I/O
 * through io_uring, then flushes data to persistent storage. Partial files are cleaned up
 * on failure via scope_exit to prevent stale test artifacts.
 */
[[nodiscard]] std::expected<PhaseRunStats, BenchmarkError> execute_write_phase(
    const std::string& filename, UringEngine& engine, const IoParams& params) {
    std::error_code pre_ec {};
    std::filesystem::remove(filename, pre_ec);

    bool write_completed = false;
    scope_exit remove_partial_file { [&filename, &write_completed]() noexcept {
        if (write_completed) { return; }

        std::error_code remove_ec {};
        std::filesystem::remove(filename, remove_ec);
    } };

    const auto phase_result = IoFile::create(filename, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR)
                                  .transform_error([](std::error_code ec) {
                                      return BenchmarkError { BenchmarkError::Phase::WritePhase,
                                          posix::SysCallError { ec, "File Creation" } };
                                  })
                                  .and_then([&engine, &params](IoFile wf) {
                                      wf.file().disable_cow();
                                      return run_write_operations(std::move(wf), engine, params);
                                  });

    if (phase_result) { write_completed = true; }

    return phase_result;
}

[[nodiscard]] std::expected<PhaseRunStats, BenchmarkError> run_read_operations(
    IoFile rf, UringEngine& engine, const IoParams& params) {
    const std::string label_read = std::format("{} Read", params.label);
    const auto ctx               = uring::ReadContext { {
                      .buffers      = params.read_buffers,
                      .fd           = rf.descriptor(),
                      .layout       = {
                          .total_blocks = params.total_bytes / params.read_block_size,
                          .block_size   = params.read_block_size,
                          .mem_stride   = params.read_mem_stride,
                          .total_bytes  = params.total_bytes,
                          .queue_depth  = params.read_queue_depth,
                      },
                      .observer     = {
                          .label        = label_read,
                          .stop         = params.stop,
                          .progress_cb  = params.progress_cb,
                          .interrupt_cb = params.interrupt_cb,
                      }
    } };

    return engine.execute_read(ctx)
        .transform_error(
            [](const uring::UringError& err) { return BenchmarkError { BenchmarkError::Phase::ReadPhase, err }; })
        .transform([](const PhaseRunStats& io_result) { return io_result; });
}

/**
 * @brief Executes the sequential read benchmark phase.
 *
 * @note Opens the previously written test file in read-only mode and submits
 * all read I/O through io_uring to measure sustained read throughput.
 */
[[nodiscard]] std::expected<PhaseRunStats, BenchmarkError> execute_read_phase(
    const std::string& filename, UringEngine& engine, const IoParams& params) {
    return IoFile::create(filename, O_RDONLY, 0)
        .transform_error([](std::error_code ec) {
            return BenchmarkError { BenchmarkError::Phase::ReadPhase, posix::SysCallError { ec, "File Open Read" } };
        })
        .and_then([&engine, &params](IoFile rf) { return run_read_operations(std::move(rf), engine, params); });
}

} // namespace

[[nodiscard]] double DiskBenchmark::get_timer_calibration_ns() noexcept {
    static const double c2ns = tsc::calibrate();
    return c2ns;
}

std::string DiskBenchmark::format_error(const BenchmarkError& err) {
    const std::string_view phase_str = [&err]() constexpr -> std::string_view {
        switch (err.phase) {
            case BenchmarkError::Phase::Configuration:
                return "Configuration";
            case BenchmarkError::Phase::SpaceCheck:
                return "Space Check";
            case BenchmarkError::Phase::BufferAllocation:
                return "Buffer Allocation";
            case BenchmarkError::Phase::EngineSetup:
                return "Engine Setup";
            case BenchmarkError::Phase::WritePhase:
                return "Write Operations";
            case BenchmarkError::Phase::ReadPhase:
                return "Read Operations";
        }
        return "Unknown";
    }();

    const std::string cause_str = std::visit(
        overloaded { [](const uring::UringError& e) -> std::string { return uring::get_error_string(e); },
            [](const affinity::IsolationError& e) -> std::string { return format_sys_error(e.ec, e.context); },
            [](const std::error_code& e) -> std::string { return e.message(); },
            [](const posix::SysCallError& e) -> std::string { return format_sys_error(e.ec, e.context); } },
        err.cause);

    return std::format("[{}] {}", phase_str, cause_str);
}

[[nodiscard]] std::expected<DiskIORunResult, BenchmarkError> DiskBenchmark::run_io_test(const BenchmarkConfig& config,
    const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const>& progress_cb,
    std::stop_token stop, const std::move_only_function<bool() const noexcept>& interrupt_cb) {
    using namespace std::chrono;

    if (config.write_queue_depth == 0 || config.read_queue_depth == 0) {
        return std::unexpected(BenchmarkError {
            BenchmarkError::Phase::Configuration, uring::UringError { uring::ConfigError::QueueDepthNotPositive } });
    }

    if (config.write_block_size == 0 || config.read_block_size == 0) {
        return std::unexpected(BenchmarkError {
            BenchmarkError::Phase::Configuration, uring::UringError { uring::ConfigError::BlockSizeNotPositive } });
    }

    if (!std::has_single_bit(config.alignment)) {
        return std::unexpected(BenchmarkError {
            BenchmarkError::Phase::Configuration, uring::UringError { uring::ConfigError::AlignmentNotPowerOfTwo } });
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

    const auto write_block_size_opt = round_up(config.write_block_size, hw.offset_align);
    if (!write_block_size_opt) {
        return std::unexpected(BenchmarkError { BenchmarkError::Phase::BufferAllocation,
            uring::UringError { uring::AllocationError::WriteBlockAlignOverflow } });
    }
    const std::uint64_t write_block_size = *write_block_size_opt;

    const auto read_block_size_opt = round_up(config.read_block_size, hw.offset_align);
    if (!read_block_size_opt) {
        return std::unexpected(BenchmarkError { BenchmarkError::Phase::BufferAllocation,
            uring::UringError { uring::AllocationError::ReadBlockAlignOverflow } });
    }
    const std::uint64_t read_block_size = *read_block_size_opt;

    /**
     * @brief Adaptive Smart Alignment:
     * - If block_size is large (>= Page), align stride to Page for maximum kernel efficiency.
     * - If block_size is small (< Page), align stride to Hardware limit to avoid memory waste.
     */
    const auto write_mem_stride_opt = (write_block_size >= get_page_size())
        ? round_up(write_block_size, get_page_size())
        : round_up(write_block_size, hw.mem_align);
    if (!write_mem_stride_opt) {
        return std::unexpected(BenchmarkError { BenchmarkError::Phase::BufferAllocation,
            uring::UringError { uring::AllocationError::WriteMemStrideOverflow } });
    }
    const auto write_mem_stride = *write_mem_stride_opt;

    const auto read_mem_stride_opt = (read_block_size >= get_page_size()) ? round_up(read_block_size, get_page_size())
                                                                          : round_up(read_block_size, hw.mem_align);
    if (!read_mem_stride_opt) {
        return std::unexpected(BenchmarkError { BenchmarkError::Phase::BufferAllocation,
            uring::UringError { uring::AllocationError::ReadMemStrideOverflow } });
    }
    const auto read_mem_stride = *read_mem_stride_opt;

    const auto raw_size = toULong(config.size_mb) * 1024ULL * 1024ULL;

    /**
     * @brief Ensure total file size is a common multiple of both block sizes
     * for clean sequential passes in both write and read phases.
     */
    const auto final_mask_res = calculate_final_mask(write_block_size, read_block_size);
    if (!final_mask_res) { return std::unexpected(final_mask_res.error()); }
    const std::size_t final_mask = *final_mask_res;

    const auto total_bytes_opt = round_up(raw_size, final_mask);
    if (!total_bytes_opt) {
        return std::unexpected(BenchmarkError { BenchmarkError::Phase::BufferAllocation,
            uring::UringError { uring::AllocationError::TotalTestSizeOverflow } });
    }
    const std::uint64_t total_bytes = *total_bytes_opt;

    scope_exit file_cleaner { [&filename]() noexcept {
        std::error_code ec {};
        std::filesystem::remove(filename, ec);
    } };

    const auto space_check_res = perform_space_check(total_bytes, dir_path);
    if (!space_check_res) { return std::unexpected(space_check_res.error()); }

    auto buffers_res = allocate_io_buffers(write_mem_stride, alignment, config, read_block_size);
    if (!buffers_res) { return std::unexpected(buffers_res.error()); }
    auto&& [write_buf, read_buffers] = std::move(*buffers_res);

    const std::uint16_t max_queue_depth = std::max(config.write_queue_depth, config.read_queue_depth);

    /** @brief Enforce strict single-core thread affinity and io-wq worker pool isolation. */
    auto engine_res { affinity::CoreAffinityGuard::make_isolated<UringEngine>(max_queue_depth)
            .transform_error([](const affinity::IsolationError& err) noexcept {
                return BenchmarkError { BenchmarkError::Phase::EngineSetup, err.ec };
            }) };
    if (!engine_res) { return std::unexpected(engine_res.error()); }
    auto&& [affinity_guard, engine] = std::move(*engine_res);

    static std::atomic<bool> warned { false };
    if (const auto res = engine.register_buffers(write_buf, read_buffers); !res && !warned.exchange(true)) {
        const bool is_enomem = std::holds_alternative<std::error_code>(res.error())
            && std::get<std::error_code>(res.error()).value() == ENOMEM;
        print_warning(std::format("Performance Hint: io_uring fixed buffers disabled ({}), using fallback.",
            is_enomem ? "Memory limit" : "System restriction"));
    }

    /**
     * @brief Callers are permitted to omit either callback, yet invoking an empty
     *        @c std::move_only_function is undefined behavior — unlike @c std::function,
     *        it provides no @c bad_function_call safety net. A guaranteed-valid
     *        fallback is therefore required before the callbacks enter the I/O pipeline.
     */
    static const std::move_only_function<bool() const noexcept> kNoopInterrupt { []() noexcept -> bool {
        return false;
    } };
    static const std::move_only_function<void(std::size_t, std::size_t, std::string_view) const> kNoopProgress {
        [](std::size_t, std::size_t, std::string_view) noexcept {}
    };

    const auto& eff_interrupt = interrupt_cb ? interrupt_cb : kNoopInterrupt;
    const auto& eff_progress  = progress_cb ? progress_cb : kNoopProgress;

    const IoParams params {
        .write_buffer      = write_buf,
        .read_buffers      = read_buffers,
        .stop              = stop,
        .progress_cb       = std::cref(eff_progress),
        .interrupt_cb      = std::cref(eff_interrupt),
        .total_bytes       = total_bytes,
        .write_block_size  = write_block_size,
        .read_block_size   = read_block_size,
        .write_mem_stride  = write_mem_stride,
        .read_mem_stride   = read_mem_stride,
        .label             = config.label,
        .write_queue_depth = config.write_queue_depth,
        .read_queue_depth  = config.read_queue_depth,
    };

    return execute_write_phase(filename, engine, params)
        .and_then([&filename, &engine, &params, &config](const PhaseRunStats& write_stats) {
            return execute_read_phase(filename, engine, params)
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

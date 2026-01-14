/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/disk_benchmark.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <new>
#include <numeric>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <system_error>

#include <sys/statvfs.h>

#include <fcntl.h>
#include <unistd.h>

#include "include/color.hpp"
#include "include/config.hpp"
#include "include/file_descriptor.hpp"
#include "include/interrupts.hpp"
#include "include/results.hpp"
#include "include/system_info.hpp"
#include "include/utils.hpp"

#ifdef USE_IO_URING
#include <liburing.h>
#endif

using namespace std::chrono;

namespace {

struct AlignedDelete {
    std::size_t alignment;

    void operator()(void* ptr) const noexcept {
        ::operator delete(ptr, std::align_val_t(alignment));
    }
};

struct FileCleaner {
    std::filesystem::path path;

    ~FileCleaner() {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::filesystem::remove(path, ec);
        }
    }
};

[[nodiscard]] std::unique_ptr<std::byte[], AlignedDelete> make_aligned_buffer(
    std::size_t size, std::size_t alignment) {
    void* ptr = ::operator new(size, std::align_val_t(alignment));
    return std::unique_ptr<std::byte[], AlignedDelete>(static_cast<std::byte*>(ptr),
                                                       AlignedDelete{alignment});
}

[[nodiscard]] std::string get_error_message(int err, std::string_view operation) {
    switch (err) {
        case ENOSPC:
            return "Storage capacity limit reached (Disk Full)";
        case EDQUOT:
            return "User disk quota exceeded";
        case EIO:
            return "Critical I/O error (Hardware failure suspected)";
        case EROFS:
            return "File system is Read-Only";
        case EACCES:
        case EPERM:
            if (operation == "create")
                return "Permission denied. Cannot create file in this directory.";
            else
                return "Permission denied during write operation.";
        case EINVAL:
            if (operation == "create")
                return "FATAL: Filesystem does NOT support O_DIRECT (Direct I/O). Test aborted to "
                       "maintain integrity.";
            else
                return "Invalid argument provided";
        default:
            return std::format(
                "Operation '{}' failed: {}", operation, std::system_category().message(err));
    }
}

#ifdef USE_IO_URING

[[nodiscard]] std::string uring_error(int rc, std::string_view op) {
    int err = -rc;
    return std::format("io_uring {} failed: {}", op, std::system_category().message(err));
}

[[nodiscard]] std::expected<void, std::string> run_uring_io(
    bool is_write,
    io_uring& ring,
    int fd,
    std::uint64_t total_blocks,
    std::uint64_t total_bytes,
    std::size_t block_size,
    std::span<std::byte> write_buffer,
    std::span<const std::unique_ptr<std::byte[], AlignedDelete>> read_buffers,
    int queue_depth,
    high_resolution_clock::time_point deadline,
    const std::function<void(std::size_t, std::size_t, std::string_view)>& progress_cb,
    std::string_view label,
    std::stop_token stop) {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    bool interrupt_requested = false;

    // Fix for race condition in circular buffer usage:
    // Track explicitly which read buffers are currently free.
    std::vector<std::size_t> free_read_indices;
    if (!is_write) {
        free_read_indices.reserve(read_buffers.size());
        for (std::size_t i = 0; i < read_buffers.size(); ++i) {
            free_read_indices.push_back(i);
        }
    }

    while (completed < total_blocks) {
        while (submitted < total_blocks &&
               (submitted - completed) < static_cast<std::uint64_t>(queue_depth)) {
            if (g_interrupted || stop.stop_requested() || interrupt_requested) {
                interrupt_requested = true;
                break;
            }

            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            if (!sqe)
                break;

            std::uint64_t offset_bytes = submitted * static_cast<std::uint64_t>(block_size);
            std::uint64_t remaining = total_bytes - offset_bytes;
            std::size_t chunk =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, block_size));
            unsigned int len = static_cast<unsigned int>(chunk);

            if (is_write) {
                // Bounds check for safety strictly
                if (chunk > write_buffer.size()) {
                    return std::unexpected("Buffer overflow detected in write preparation");
                }
                io_uring_prep_write(sqe, fd, write_buffer.data(), len, offset_bytes);
                io_uring_sqe_set_data64(sqe, static_cast<__u64>(len));
            } else {
                if (free_read_indices.empty()) {
                    // Should theoretically not happen if queue_depth matches buffer count
                    return std::unexpected("Logic Error: No free read buffers available");
                }
                size_t buf_idx = free_read_indices.back();
                free_read_indices.pop_back();

                if (buf_idx >= read_buffers.size()) {
                    return std::unexpected("Read buffer index out of range");
                }
                io_uring_prep_read(sqe, fd, read_buffers[buf_idx].get(), len, offset_bytes);

                // Pack index (High 32) and length (Low 32) to track buffer usage across async
                // completions
                std::uint64_t user_data =
                    (static_cast<std::uint64_t>(buf_idx) << 32) | static_cast<std::uint32_t>(len);
                io_uring_sqe_set_data64(sqe, user_data);
            }

            submitted++;
        }

        int submit_rc = io_uring_submit(&ring);
        if (submit_rc < 0) {
            return std::unexpected(uring_error(submit_rc, "submit"));
        }

        io_uring_cqe* cqe = nullptr;
        int wait_rc = io_uring_wait_cqe(&ring, &cqe);

        if (wait_rc < 0) {
            if (wait_rc == -EINTR) {
                if (g_interrupted || stop.stop_requested()) {
                    interrupt_requested = true;
                }
                continue;
            }
            return std::unexpected(uring_error(wait_rc, "wait"));
        }

        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring, head, cqe) {
            count++;
            uint64_t user_data = io_uring_cqe_get_data64(cqe);
            int expected_len = static_cast<int>(user_data & 0xFFFFFFFF);

            if (!is_write) {
                size_t buf_idx = static_cast<size_t>(user_data >> 32);
                free_read_indices.push_back(buf_idx);
            }

            if (cqe->res < 0) {
                io_uring_cq_advance(&ring, count);
                std::string op = is_write ? "write" : "read";
                return std::unexpected("Disk Test failed: " + get_error_message(-cqe->res, op));
            }

            if (cqe->res != expected_len) {
                io_uring_cq_advance(&ring, count);
                std::string op = is_write ? "write" : "read";
                return std::unexpected(
                    std::format("Disk Test failed: Partial {} (expected {} bytes, got {})",
                                op,
                                expected_len,
                                cqe->res));
            }

            ++completed;
            if (progress_cb && completed % 2 == 0) {
                progress_cb(static_cast<std::size_t>(completed),
                            static_cast<std::size_t>(total_blocks),
                            label);
            }
        }

        io_uring_cq_advance(&ring, count);

        if (interrupt_requested && completed >= submitted) {
            return std::unexpected("Operation interrupted by user");
        }

        if (high_resolution_clock::now() > deadline) {
            interrupt_requested = true;
            if (completed >= submitted) {
                return std::unexpected("Disk Test timed out (operation took too long)");
            }
        }
    }

    return {};
}

#endif

// Helper to attempt opening file with increasingly relaxed consistency flags
[[nodiscard]] std::pair<FileDescriptor, int> open_benchmark_file(const std::string& path,
                                                      int flags,
                                                      mode_t mode) {
    int fd = -1;
    // 0=none, 1=DIRECT+DSYNC, 2=DIRECT, 3=DSYNC, 4=Buffered

#ifdef O_DIRECT
    // Try 1: O_DIRECT | O_DSYNC (Best)
    fd = ::open(path.c_str(), flags | O_DIRECT | O_DSYNC, mode);
    if (fd >= 0)
        return {FileDescriptor(fd), 1};

    // If verification fails for reasons other than "Invalid Argument" (which implies
    // flags not supported), we should stop and report that specific error.
    if (errno != EINVAL)
        return {FileDescriptor(), 0};

    // Try 2: O_DIRECT (Good)
    fd = ::open(path.c_str(), flags | O_DIRECT, mode);
    if (fd >= 0)
        return {FileDescriptor(fd), 2};

    if (errno != EINVAL)
        return {FileDescriptor(), 0};
#endif

    // Try 3: O_DSYNC (Reliable but maybe cached read)
    // We reach here if O_DIRECT is undefined OR if previous attempts failed with EINVAL.
    fd = ::open(path.c_str(), flags | O_DSYNC, mode);
    if (fd >= 0)
        return {FileDescriptor(fd), 3};

    if (errno != EINVAL)
        return {FileDescriptor(), 0};

    // Try 4: Buffered (Fallback)
    fd = ::open(path.c_str(), flags, mode);
    if (fd >= 0)
        return {FileDescriptor(fd), 4};

    return {FileDescriptor(), 0};
}

void print_storage_warning(int mode, bool is_read) {
    if (mode == 1)
        return;  // Best case

    const char* op_type = is_read ? "read" : "write";
    const char* reason = "";

    switch (mode) {
        case 2:
            reason = "O_DSYNC not supported";
            break;
        case 3:
            reason = "O_DIRECT not supported";
            break;
        case 4:
            reason = "O_DIRECT/O_DSYNC not supported";
            break;
        default:
            return;
    }

    std::print(stderr,
               "{}Warning: {}. Benchmark {} results may be influenced by RAM cache.{}\n",
               Color::YELLOW,
               reason,
               op_type,
               Color::RESET);
}

}  // namespace

std::expected<DiskIORunResult, std::string> DiskBenchmark::run_io_test(
    int size_mb,
    std::string_view label,
    const std::function<void(std::size_t, std::size_t, std::string_view)>& progress_cb,
    std::stop_token stop) {
    const std::string filename = std::format("{}.{}", Config::TEST_FILENAME, getpid());
    FileCleaner cleaner{filename};

    const size_t write_block_size = Config::IO_WRITE_BLOCK_SIZE;
    const size_t read_block_size = Config::IO_READ_BLOCK_SIZE;
    const int queue_depth_write = std::max(1, Config::IO_WRITE_QUEUE_DEPTH);
    const int queue_depth_read = std::max(1, Config::IO_READ_QUEUE_DEPTH);

    auto current_path = std::filesystem::current_path();
    std::uint64_t required = static_cast<std::uint64_t>(size_mb) * 1024 * 1024;

    if (!is_disk_space_available(current_path, required)) {
        return std::unexpected("Insufficient free space for disk test (needs " +
                               format_bytes(required) + ")");
    }

    auto buffer = make_aligned_buffer(write_block_size, Config::IO_ALIGNMENT);

    // Modern C++20 range generation
    constexpr unsigned int RNG_MULTIPLIER = 0x9E3779B1u;
    std::ranges::generate(std::span{buffer.get(), write_block_size}, [i = 0u]() mutable {
        return std::byte{static_cast<unsigned char>(i++ * RNG_MULTIPLIER)};
    });

    std::vector<std::unique_ptr<std::byte[], AlignedDelete>> read_buffers;
    read_buffers.reserve(static_cast<size_t>(queue_depth_read));
    for (int k = 0; k < queue_depth_read; ++k) {
        read_buffers.push_back(make_aligned_buffer(read_block_size, Config::IO_ALIGNMENT));
        std::ranges::generate(
            std::span{read_buffers.back().get(), read_block_size}, [i = 0u]() mutable {
                return std::byte{static_cast<unsigned char>(i++ * RNG_MULTIPLIER)};
            });
    }

    auto start = high_resolution_clock::now();
    auto deadline = start + std::chrono::seconds(Config::DISK_BENCHMARK_MAX_SECONDS);
    const std::uint64_t total_bytes = static_cast<std::uint64_t>(size_mb) * 1024 * 1024;
    const std::uint64_t total_write_blocks =
        (total_bytes + write_block_size - 1) / write_block_size;
    const std::uint64_t total_read_blocks = (total_bytes + read_block_size - 1) / read_block_size;

    {
        const std::string write_label = std::string(label) + " Write";

        std::error_code ec;
        std::filesystem::remove(filename, ec);

        // Security: Use 0600 (S_IRUSR | S_IWUSR) so only owner can read/write the benchmark file
        const int base_flags = O_WRONLY | O_CREAT | O_TRUNC | O_EXCL;

#ifndef O_DIRECT
        return std::unexpected("FATAL: O_DIRECT is not available on this platform compilation.");
#endif

        auto [fd, success_mode] = open_benchmark_file(filename, base_flags, 0600);

        if (!fd) {
            return std::unexpected(get_error_message(errno, "create"));
        }

        print_storage_warning(success_mode, false);

        {
            off_t prealloc_bytes = static_cast<off_t>(total_bytes);
            int prealloc_rc = ::posix_fallocate(fd.get(), 0, prealloc_bytes);
            if (prealloc_rc != 0 && prealloc_rc != EINVAL && prealloc_rc != ENOTSUP) {
                return std::unexpected(std::format("Preallocation failed: {}",
                                                   std::system_category().message(prealloc_rc)));
            }
        }

#ifdef USE_IO_URING
        io_uring ring{};
        int ret = io_uring_queue_init(queue_depth_write, &ring, 0);
        if (ret != 0) {
            return std::unexpected(std::format("FATAL: io_uring init failed: {}. Kernel too old?",
                                               std::system_category().message(-ret)));
        }

        auto res = run_uring_io(true,
                                ring,
                                fd.get(),
                                total_write_blocks,
                                total_bytes,
                                write_block_size,
                                std::span{buffer.get(), write_block_size},
                                {},
                                queue_depth_write,
                                deadline,
                                progress_cb,
                                write_label,
                                stop);
        io_uring_queue_exit(&ring);
        if (!res)
            return std::unexpected(res.error());
#else
        return std::unexpected(
            "FATAL: Binary compiled without io_uring support. Cannot benchmark.");
#endif

        if (progress_cb)
            progress_cb(static_cast<std::size_t>(total_write_blocks),
                        static_cast<std::size_t>(total_write_blocks),
                        write_label);

        if (::fdatasync(fd.get()) == -1) {
            return std::unexpected("Disk sync failed: " + get_error_message(errno, "sync"));
        }

        ::posix_fadvise(fd.get(), 0, 0, POSIX_FADV_DONTNEED);
    }

    auto end_write = high_resolution_clock::now();
    duration<double> diff_write = end_write - start;
    double write_speed =
        diff_write.count() <= 0 ? 0.0 : static_cast<double>(size_mb) / diff_write.count();

    // Read Test
    int rd_flags = O_RDONLY;
    auto [read_fd, read_success_mode] =
        open_benchmark_file(filename, rd_flags, 0);  // Mode ignored for O_RDONLY

    if (!read_fd) {
        return std::unexpected(get_error_message(errno, "open/read"));
    }

    print_storage_warning(read_success_mode, true);
    auto read_start = high_resolution_clock::now();

    deadline = read_start + std::chrono::seconds(Config::DISK_BENCHMARK_MAX_SECONDS);
    const std::string read_label = std::string(label) + " Read";

#ifdef USE_IO_URING
    io_uring ring{};
    int ret = io_uring_queue_init(queue_depth_read, &ring, 0);
    if (ret != 0) {
        return std::unexpected(std::format("FATAL: io_uring init failed for read: {}",
                                           std::system_category().message(-ret)));
    }

    auto res = run_uring_io(false,
                            ring,
                            read_fd.get(),
                            total_read_blocks,
                            total_bytes,
                            read_block_size,
                            std::span<std::byte>{},
                            read_buffers,
                            queue_depth_read,
                            deadline,
                            progress_cb,
                            read_label,
                            stop);
    io_uring_queue_exit(&ring);
    if (!res)
        return std::unexpected(res.error());
#endif

    if (progress_cb)
        progress_cb(static_cast<size_t>(total_read_blocks),
                    static_cast<size_t>(total_read_blocks),
                    read_label);

    auto end_read = high_resolution_clock::now();
    duration<double> diff_read = end_read - read_start;
    double read_speed =
        diff_read.count() <= 0 ? 0.0 : static_cast<double>(size_mb) / diff_read.count();

    return DiskIORunResult{std::string(label), write_speed, read_speed};
}

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

std::unique_ptr<std::byte[], AlignedDelete> make_aligned_buffer(std::size_t size,
                                                                std::size_t alignment) {
    void* ptr = ::operator new(size, std::align_val_t(alignment));
    return std::unique_ptr<std::byte[], AlignedDelete>(static_cast<std::byte*>(ptr),
                                                       AlignedDelete{alignment});
}

std::string get_error_message(int err, std::string_view operation) {
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

std::string uring_error(int rc, std::string_view op) {
    int err = -rc;
    return std::format("io_uring {} failed: {}", op, std::system_category().message(err));
}

std::expected<void, std::string> run_uring_io(
    bool is_write,
    io_uring& ring,
    int fd,
    std::uint64_t total_blocks,
    std::uint64_t total_bytes,
    std::size_t block_size,
    std::byte* write_buffer,
    const std::vector<std::unique_ptr<std::byte[], AlignedDelete>>& read_buffers,
    int queue_depth,
    high_resolution_clock::time_point deadline,
    const std::function<void(std::size_t, std::size_t, std::string_view)>& progress_cb,
    std::string_view label,
    std::stop_token stop) {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    bool interrupt_requested = false;

    while (completed < total_blocks) {
        while (submitted < total_blocks &&
               (submitted - completed) < static_cast<std::uint64_t>(queue_depth)) {
            if (g_interrupted || stop.stop_requested()) {
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
                io_uring_prep_write(sqe, fd, write_buffer, len, offset_bytes);
            } else {
                auto* buf = read_buffers[static_cast<std::size_t>(
                                             submitted % static_cast<std::uint64_t>(queue_depth))]
                                .get();
                io_uring_prep_read(sqe, fd, buf, len, offset_bytes);
            }

            io_uring_sqe_set_data64(sqe, static_cast<__u64>(len));

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

            auto expected_len = static_cast<int>(io_uring_cqe_get_data64(cqe));

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

}  // namespace

std::expected<DiskIORunResult, std::string> DiskBenchmark::run_io_test(
    int size_mb,
    std::string_view label,
    const std::function<void(std::size_t, std::size_t, std::string_view)>& progress_cb,
    std::stop_token stop) {
    const std::string filename(Config::TEST_FILENAME);
    FileCleaner cleaner{filename};

    const size_t write_block_size = Config::IO_WRITE_BLOCK_SIZE;
    const size_t read_block_size = Config::IO_READ_BLOCK_SIZE;
    const int queue_depth_write = std::max(1, Config::IO_WRITE_QUEUE_DEPTH);
    const int queue_depth_read = std::max(1, Config::IO_READ_QUEUE_DEPTH);

    struct statvfs vfs {};
    if (::statvfs(".", &vfs) == 0) {
        std::uint64_t available = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        std::uint64_t required = static_cast<std::uint64_t>(size_mb) * 1024 * 1024;
        if (available < required) {
            return std::unexpected("Insufficient free space for disk test (needs " +
                                   format_bytes(required) + ")");
        }
    }

    auto buffer = make_aligned_buffer(write_block_size, Config::IO_ALIGNMENT);

    std::ranges::generate(std::span{buffer.get(), write_block_size}, [i = 0u]() mutable {
        return std::byte{static_cast<unsigned char>(i++ * 0x9E3779B1u)};
    });

    std::vector<std::unique_ptr<std::byte[], AlignedDelete>> read_buffers;
    read_buffers.reserve(static_cast<size_t>(queue_depth_read));
    for (int k = 0; k < queue_depth_read; ++k) {
        read_buffers.push_back(make_aligned_buffer(read_block_size, Config::IO_ALIGNMENT));
        std::ranges::generate(std::span{read_buffers.back().get(), read_block_size},
                              [i = 0u]() mutable {
                                  return std::byte{static_cast<unsigned char>(i++ * 0x9E3779B1u)};
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

        const int base_flags = O_WRONLY | O_CREAT | O_TRUNC | O_EXCL;
        int fd_raw = -1;

#ifdef O_DIRECT
        fd_raw = ::open(filename.c_str(), base_flags | O_DIRECT | O_DSYNC, 0644);
        if (fd_raw < 0 && errno == EINVAL) {
            fd_raw = ::open(filename.c_str(), base_flags | O_DIRECT, 0644);
            if (fd_raw >= 0) {
                std::print(stderr,
                           "{}Warning: O_DSYNC not supported. Results may be influenced by RAM "
                           "cache.{}\n",
                           Color::YELLOW,
                           Color::RESET);
            } else if (errno == EINVAL) {
                fd_raw = ::open(filename.c_str(), base_flags | O_DSYNC, 0644);
                if (fd_raw >= 0) {
                    std::print(stderr,
                               "{}Warning: O_DIRECT not supported. Results may be influenced by "
                               "RAM cache.{}\n",
                               Color::YELLOW,
                               Color::RESET);
                } else if (errno == EINVAL) {
                    fd_raw = ::open(filename.c_str(), base_flags, 0644);
                    if (fd_raw >= 0) {
                        std::print(stderr,
                                   "{}Warning: O_DIRECT/O_DSYNC not supported. Results WILL be "
                                   "influenced by RAM cache.{}\n",
                                   Color::YELLOW,
                                   Color::RESET);
                    }
                }
            }
        }
#else
        return std::unexpected("FATAL: O_DIRECT is not available on this platform compilation.");
#endif

        if (fd_raw < 0) {
            return std::unexpected(get_error_message(errno, "create"));
        }

        {
            off_t prealloc_bytes = static_cast<off_t>(total_bytes);
            int prealloc_rc = ::posix_fallocate(fd_raw, 0, prealloc_bytes);
            if (prealloc_rc != 0 && prealloc_rc != EINVAL && prealloc_rc != ENOTSUP) {
                ::close(fd_raw);
                return std::unexpected(std::format("Preallocation failed: {}",
                                                   std::system_category().message(prealloc_rc)));
            }
        }

        FileDescriptor fd(fd_raw);

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
                                buffer.get(),
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

    int rd_flags = O_RDONLY;
#ifdef O_DIRECT
    int rd_raw = ::open(filename.c_str(), rd_flags | O_DIRECT);
    if (rd_raw < 0 && errno == EINVAL) {
        rd_raw = ::open(filename.c_str(), rd_flags);
        if (rd_raw >= 0) {
            std::print(
                stderr,
                "{}Warning: O_DIRECT not supported for read. Results may be influenced by RAM "
                "cache.{}\n",
                Color::YELLOW,
                Color::RESET);
        }
    }
#else
    int rd_raw = ::open(filename.c_str(), rd_flags);
#endif

    if (rd_raw < 0) {
        return std::unexpected(get_error_message(errno, "open/read"));
    }

    FileDescriptor read_fd(rd_raw);
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
                            nullptr,
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
#include "include/disk_benchmark.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <atomic>
#include <functional>
#include <memory>
#include <new>
#include <future>
#include <ranges>
#include <span>
#include <stop_token>
#include <system_error>
#include <stdexcept>
#include <format>
#include <cerrno>
#include <numeric>

#include <sys/statvfs.h>

#include <fcntl.h>
#include <unistd.h>

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

std::unique_ptr<std::byte[], AlignedDelete> make_aligned_buffer(std::size_t size, std::size_t alignment) {
    void* ptr = ::operator new(size, std::align_val_t(alignment));
    return std::unique_ptr<std::byte[], AlignedDelete>(
        static_cast<std::byte*>(ptr),
        AlignedDelete{alignment}
    );
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
                return "Invalid arguments (O_DIRECT not supported on this filesystem?)";
            else
                return "Invalid argument provided";
        default:     
            return std::format("Operation '{}' failed: {}", operation, std::system_category().message(err));
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

    while (completed < total_blocks) {
        while (submitted < total_blocks && (submitted - completed) < static_cast<std::uint64_t>(queue_depth)) {
            if (g_interrupted || stop.stop_requested()) {
                return std::unexpected("Operation interrupted by user");
            }

            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;

            std::uint64_t offset_bytes = submitted * static_cast<std::uint64_t>(block_size);
            std::uint64_t remaining = total_bytes - offset_bytes;
            std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, block_size));

            unsigned int len = static_cast<unsigned int>(chunk);
            if (is_write) {
                io_uring_prep_write(sqe, fd, write_buffer, len, offset_bytes);
            } else {
                auto* buf = read_buffers[static_cast<std::size_t>(submitted % static_cast<std::uint64_t>(queue_depth))].get();
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
            return std::unexpected(uring_error(wait_rc, "wait"));
        }

        if (cqe) {
            auto expected_len = static_cast<int>(io_uring_cqe_get_data64(cqe));

            if (cqe->res < 0) {
                std::string op = is_write ? "write" : "read";
                io_uring_cqe_seen(&ring, cqe);
                return std::unexpected("Benchmark failed: " + get_error_message(-cqe->res, op));
            }

            if (cqe->res != expected_len) {
                io_uring_cqe_seen(&ring, cqe);
                std::string op = is_write ? "write" : "read";
                return std::unexpected(std::format("Benchmark failed: Partial {} (expected {} bytes, got {})", op, expected_len, cqe->res));
            }

            io_uring_cqe_seen(&ring, cqe);
            ++completed;
            if (progress_cb && completed % 2 == 0) {
                progress_cb(static_cast<std::size_t>(completed), static_cast<std::size_t>(total_blocks), label);
            }
        }

        if (high_resolution_clock::now() > deadline) {
            return std::unexpected("Benchmark timed out (operation took too long)");
        }
    }

    return {};
}

#endif

} // namespace

std::expected<DiskIORunResult, std::string> DiskBenchmark::run_io_test(
    int size_mb,
    std::string_view label,
    const std::function<void(std::size_t, std::size_t, std::string_view)>& progress_cb,
    std::stop_token stop) {
    
    const std::string filename(Config::BENCH_FILENAME);
    FileCleaner cleaner{filename};

    const size_t write_block_size = Config::IO_WRITE_BLOCK_SIZE;
    const size_t read_block_size = Config::IO_READ_BLOCK_SIZE;
    const int queue_depth_write = std::max(1, Config::IO_WRITE_QUEUE_DEPTH);
    const int queue_depth_read = std::max(1, Config::IO_READ_QUEUE_DEPTH);

    struct statvfs vfs{};
    if (::statvfs(".", &vfs) == 0) {
        std::uint64_t available = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        std::uint64_t required = static_cast<std::uint64_t>(size_mb) * 1024 * 1024;
        if (available < required) {
            return std::unexpected("Insufficient free space for disk benchmark (needs " + format_bytes(required) + ")");
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
        std::ranges::generate(std::span{read_buffers.back().get(), read_block_size}, [i = 0u]() mutable { 
            return std::byte{static_cast<unsigned char>(i++ * 0x9E3779B1u)}; 
        });
    }

    auto start = high_resolution_clock::now();
    auto deadline = start + std::chrono::seconds(Config::DISK_BENCH_MAX_SECONDS);
    const std::uint64_t total_bytes = static_cast<std::uint64_t>(size_mb) * 1024 * 1024;
    const std::uint64_t total_write_blocks = (total_bytes + write_block_size - 1) / write_block_size;
    const std::uint64_t total_read_blocks  = (total_bytes + read_block_size - 1) / read_block_size;

    {
        const std::string write_label = std::string(label) + " Write";

        int flags = O_WRONLY | O_CREAT | O_TRUNC | O_EXCL;
        #ifdef O_DIRECT
        flags |= O_DIRECT;
        #endif

        int fd_raw = ::open(filename.c_str(), flags, 0644);
        if (fd_raw < 0 && errno == EINVAL) {
            #ifdef O_DIRECT
            flags &= ~O_DIRECT;
            #endif
            flags |= O_SYNC;
            fd_raw = ::open(filename.c_str(), flags, 0644);
        }
        if (fd_raw < 0) {
            return std::unexpected(get_error_message(errno, "create"));
        }

        {
            off_t prealloc_bytes = static_cast<off_t>(total_bytes);
            int prealloc_rc = ::posix_fallocate(fd_raw, 0, prealloc_bytes);
            if (prealloc_rc != 0 && prealloc_rc != EINVAL && prealloc_rc != ENOTSUP) {
                ::close(fd_raw);
                return std::unexpected(std::format("Preallocation failed: {}", std::system_category().message(prealloc_rc)));
            }
        }

        FileDescriptor fd(fd_raw);

#ifdef USE_IO_URING
        bool used_uring = false;
        if (Config::IO_URING_ENABLED) {
            io_uring ring{};
            if (io_uring_queue_init(queue_depth_write, &ring, 0) == 0) {
                static const std::vector<std::unique_ptr<std::byte[], AlignedDelete>> empty_buffers;
                auto res = run_uring_io(
                    true,
                    ring,
                    fd.get(),
                    total_write_blocks,
                    total_bytes,
                    write_block_size,
                    buffer.get(),
                    empty_buffers,
                    queue_depth_write,
                    deadline,
                    progress_cb,
                    write_label,
                    stop);
                io_uring_queue_exit(&ring);
                if (!res) return std::unexpected(res.error());
                used_uring = true;
            }
        }
        if (!used_uring) {
#endif
            std::vector<std::future<std::expected<void, std::string>>> in_flight;
            in_flight.reserve(static_cast<std::size_t>(queue_depth_write));

            auto submit_write = [&](std::uint64_t block_idx) {
                return std::async(std::launch::async, [&, block_idx]() -> std::expected<void, std::string> {
                    if (g_interrupted || stop.stop_requested()) {
                        return std::unexpected("Operation interrupted by user");
                    }

                    off_t offset = static_cast<off_t>(block_idx * write_block_size);
                    std::uint64_t remaining = total_bytes - static_cast<std::uint64_t>(offset);
                    size_t chunk = static_cast<size_t>(std::min<std::uint64_t>(remaining, write_block_size));

                    ssize_t written = 0;
                    do {
                        written = ::pwrite(fd.get(), buffer.get(), chunk, offset);
                    } while (written < 0 && errno == EINTR);
                    if (written != static_cast<ssize_t>(chunk)) {
                        return std::unexpected("Benchmark failed: " + get_error_message(errno, "write"));
                    }

                    if (high_resolution_clock::now() > deadline) {
                        return std::unexpected("Benchmark timed out (operation took too long)");
                    }
                    return {};
                });
            };

            std::uint64_t completed = 0;
            for (std::uint64_t i = 0; i < total_write_blocks; ++i) {
                if (g_interrupted) return std::unexpected("Operation interrupted by user");
                if (stop.stop_requested()) return std::unexpected("Operation interrupted by user request.");

                in_flight.push_back(submit_write(i));

                if (in_flight.size() >= static_cast<std::size_t>(queue_depth_write) || i + 1 == total_write_blocks) {
                    for (auto& fut : in_flight) {
                        auto res = fut.get();
                        if (!res) return std::unexpected(res.error());
                        ++completed;
                        if (progress_cb && completed % 2 == 0) {
                            progress_cb(static_cast<std::size_t>(completed), static_cast<std::size_t>(total_write_blocks), write_label);
                        }
                    }
                    in_flight.clear();
                }
            }
#ifdef USE_IO_URING
        }
#endif

        if (progress_cb) progress_cb(static_cast<std::size_t>(total_write_blocks), static_cast<std::size_t>(total_write_blocks), write_label);

        if (::fdatasync(fd.get()) == -1) {
            return std::unexpected("Disk sync failed: " + get_error_message(errno, "sync"));
        }

        if (::posix_fadvise(fd.get(), 0, 0, POSIX_FADV_DONTNEED) != 0) {
            // Ignore error. This is just an optimization hint.
            // If it fails, benchmark continues anyway.
        }
    }

    auto end_write = high_resolution_clock::now();
    duration<double> diff_write = end_write - start;
    double write_speed = diff_write.count() <= 0 ? 0.0 : static_cast<double>(size_mb) / diff_write.count();

    int rd_flags = O_RDONLY;
    #ifdef O_DIRECT
    rd_flags |= O_DIRECT;
    #endif

    int rd_raw = ::open(filename.c_str(), rd_flags);
    if (rd_raw < 0 && errno == EINVAL) {
        #ifdef O_DIRECT
        rd_flags &= ~O_DIRECT;
        #endif
        rd_raw = ::open(filename.c_str(), rd_flags);
    }
    if (rd_raw < 0) {
        return std::unexpected(get_error_message(errno, "open/read"));
    }

    FileDescriptor read_fd(rd_raw);
    auto read_start = high_resolution_clock::now();

    deadline = read_start + std::chrono::seconds(Config::DISK_BENCH_MAX_SECONDS);

    const std::string read_label = std::string(label) + " Read";

#ifdef USE_IO_URING
    bool read_used_uring = false;
    if (Config::IO_URING_ENABLED) {
        io_uring ring{};
        if (io_uring_queue_init(queue_depth_read, &ring, 0) == 0) {
            auto res = run_uring_io(
                false,
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
            if (!res) return std::unexpected(res.error());
            read_used_uring = true;
        }
    }
    if (!read_used_uring) {
#endif
        std::vector<std::future<std::expected<void, std::string>>> read_futs;
        read_futs.reserve(static_cast<std::size_t>(queue_depth_read));

        auto submit_read = [&](std::uint64_t block_idx) {
            return std::async(std::launch::async, [&, block_idx]() -> std::expected<void, std::string> {
                if (g_interrupted || stop.stop_requested()) {
                    return std::unexpected("Operation interrupted by user");
                }

                std::uint64_t offset_bytes = block_idx * static_cast<std::uint64_t>(read_block_size);
                std::uint64_t remaining = total_bytes - offset_bytes;
                size_t chunk = static_cast<size_t>(std::min<std::uint64_t>(remaining, read_block_size));

                auto buf_slot = static_cast<std::size_t>(block_idx % static_cast<std::uint64_t>(queue_depth_read));
                auto* buf = read_buffers[buf_slot].get();

                ssize_t r = 0;
                do {
                    r = ::pread(read_fd.get(), buf, chunk, static_cast<off_t>(offset_bytes));
                } while (r < 0 && errno == EINTR);

                if (r < 0) {
                    return std::unexpected("Benchmark failed: " + get_error_message(errno, "read"));
                }
                if (r == 0) {
                    return std::unexpected("Benchmark failed: Unexpected EOF during read");
                }

                if (high_resolution_clock::now() > deadline) {
                    return std::unexpected("Benchmark timed out (operation took too long)");
                }
                return {};
            });
        };

        std::uint64_t completed_read = 0;
        for (std::uint64_t i = 0; i < total_read_blocks; ++i) {
            if (g_interrupted) return std::unexpected("Operation interrupted by user");
            if (stop.stop_requested()) return std::unexpected("Operation interrupted by user request.");

            read_futs.push_back(submit_read(i));

            if (read_futs.size() >= static_cast<std::size_t>(queue_depth_read) || i + 1 == total_read_blocks) {
                for (auto& fut : read_futs) {
                    auto res = fut.get();
                    if (!res) return std::unexpected(res.error());
                    ++completed_read;
                    if (progress_cb && completed_read % 2 == 0) {
                        progress_cb(static_cast<size_t>(completed_read), static_cast<size_t>(total_read_blocks), read_label);
                    }
                }
                read_futs.clear();
            }
        }

        if (progress_cb) progress_cb(static_cast<size_t>(total_read_blocks), static_cast<size_t>(total_read_blocks), read_label);
#ifdef USE_IO_URING
    }
#endif

    auto end_read = high_resolution_clock::now();
    duration<double> diff_read = end_read - read_start;
    double read_speed = diff_read.count() <= 0 ? 0.0 : static_cast<double>(size_mb) / diff_read.count();

    return DiskIORunResult{std::string(label), write_speed, read_speed};
}
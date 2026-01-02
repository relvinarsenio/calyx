#include "include/disk_benchmark.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stop_token>
#include <system_error>
#include <stdexcept>
#include <format>
#include <cerrno>

#include <sys/statvfs.h>

#include <fcntl.h>
#include <unistd.h>

#include "include/config.hpp"
#include "include/file_descriptor.hpp"
#include "include/interrupts.hpp"
#include "include/results.hpp"
#include "include/utils.hpp"

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

}

std::expected<DiskIORunResult, std::string> DiskBenchmark::run_io_test(
    int size_mb,
    std::string_view label,
    const std::function<void(std::size_t, std::size_t, std::string_view)>& progress_cb,
    std::stop_token stop) {
    
    const std::string filename(Config::BENCH_FILENAME);
    FileCleaner cleaner{filename};

    const size_t block_size = Config::IO_BLOCK_SIZE;

    struct statvfs vfs{};
    if (::statvfs(".", &vfs) == 0) {
        std::uint64_t available = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        std::uint64_t required = static_cast<std::uint64_t>(size_mb) * 1024 * 1024;
        if (available < required) {
            return std::unexpected("Insufficient free space for disk benchmark (needs " + format_bytes(required) + ")");
        }
    }

    auto buffer = make_aligned_buffer(block_size, Config::IO_ALIGNMENT);
    
    std::ranges::fill(std::span{buffer.get(), block_size}, std::byte{0});

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

    auto start = high_resolution_clock::now();
    auto deadline = start + std::chrono::seconds(Config::DISK_BENCH_MAX_SECONDS);
    size_t blocks = (static_cast<size_t>(size_mb) * 1024 * 1024) / block_size;

    {
        FileDescriptor fd(fd_raw);

        const std::string write_label = std::string(label) + " Write";
        for (size_t i = 0; i < blocks; ++i) {
            if (g_interrupted) {
                return std::unexpected("Operation interrupted by user");
            }
            
            if (stop.stop_requested()) {
                return std::unexpected("Operation interrupted by user request.");
            }
            
            ssize_t written = ::write(fd.get(), buffer.get(), block_size);
            
            if (written != static_cast<ssize_t>(block_size)) {
                return std::unexpected("Benchmark failed: " + get_error_message(errno, "write"));
            }

            if (high_resolution_clock::now() > deadline) {
                return std::unexpected("Benchmark timed out (operation took too long)");
            }

            if (progress_cb && i % 2 == 0) progress_cb(i + 1, blocks, write_label);
        }
        if (progress_cb) progress_cb(blocks, blocks, write_label);

        if (::fdatasync(fd.get()) == -1) {
            return std::unexpected("Disk sync failed: " + get_error_message(errno, "sync"));
        }

        ::fsync(fd.get());
    }
    int advise_fd = ::open(filename.c_str(), O_RDONLY);
    if (advise_fd >= 0) {
        ::posix_fadvise(advise_fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(advise_fd);
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

    FileDescriptor rd_fd(rd_raw);
    auto read_start = high_resolution_clock::now();
    std::uint64_t bytes_to_read = static_cast<std::uint64_t>(size_mb) * 1024 * 1024;

    const std::string read_label = std::string(label) + " Read";
    std::uint64_t blocks_read = 0;
    const std::uint64_t total_blocks_read = (bytes_to_read + block_size - 1) / block_size;

    while (bytes_to_read > 0) {
        if (g_interrupted) return std::unexpected("Operation interrupted by user");
        if (stop.stop_requested()) return std::unexpected("Operation interrupted by user request.");

        size_t chunk = static_cast<size_t>(std::min<std::uint64_t>(bytes_to_read, block_size));
        ssize_t r = ::read(rd_fd.get(), buffer.get(), chunk);
        if (r < 0) {
            if (errno == EINTR) continue;
            return std::unexpected("Benchmark failed: " + get_error_message(errno, "read"));
        }
        if (r == 0) {
            return std::unexpected("Benchmark failed: Unexpected EOF during read");
        }
        bytes_to_read -= static_cast<std::uint64_t>(r);

        if (high_resolution_clock::now() > deadline) {
            return std::unexpected("Benchmark timed out (operation took too long)");
        }

        ++blocks_read;
        if (progress_cb && blocks_read % 2 == 0) {
            progress_cb(static_cast<size_t>(blocks_read), static_cast<size_t>(total_blocks_read), read_label);
        }
    }

    if (progress_cb) progress_cb(static_cast<size_t>(total_blocks_read), static_cast<size_t>(total_blocks_read), read_label);

    auto end_read = high_resolution_clock::now();
    duration<double> diff_read = end_read - read_start;
    double read_speed = diff_read.count() <= 0 ? 0.0 : static_cast<double>(size_mb) / diff_read.count();

    return DiskIORunResult{std::string(label), write_speed, read_speed};
}
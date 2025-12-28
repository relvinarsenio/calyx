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

#include <fcntl.h>
#include <unistd.h>

#include "include/config.hpp"
#include "include/file_descriptor.hpp"
#include "include/interrupts.hpp"
#include "include/results.hpp"

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

std::expected<DiskRunResult, std::string> DiskBenchmark::run_write_test(
    int size_mb,
    std::string_view label,
    const std::function<void(std::size_t, std::size_t, std::string_view)>& progress_cb,
    std::stop_token stop) {
    
    const std::string filename(Config::BENCH_FILENAME);
    FileCleaner cleaner{filename};

    const size_t block_size = Config::IO_BLOCK_SIZE;

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
         fd_raw = ::open(filename.c_str(), flags, 0644);
    }

    if (fd_raw < 0) {
        return std::unexpected(get_error_message(errno, "create"));
    }

    FileDescriptor fd(fd_raw);

    auto start = high_resolution_clock::now();
    size_t blocks = (size_t(size_mb) * 1024 * 1024) / block_size;

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

        if (progress_cb && i % 2 == 0) progress_cb(i + 1, blocks, label);
    }
    if (progress_cb) progress_cb(blocks, blocks, label);

    if (::fdatasync(fd.get()) == -1) {
        return std::unexpected("Disk sync failed: " + get_error_message(errno, "sync"));
    }

    auto end = high_resolution_clock::now();
    
    duration<double> diff = end - start;
    double speed = diff.count() <= 0 ? 0.0 : static_cast<double>(size_mb) / diff.count();
    
    return DiskRunResult{std::string(label), speed};
}
# Bench - Modern Linux Server Benchmark

**Bench** is a high-performance Linux server benchmarking tool written in **Modern C++ (C++23)**. It provides accurate, detailed, and memory-safe performance metrics.

Unlike traditional bash scripts, Bench parses kernel interfaces (`/proc`, `/sys`) directly and utilizes native system calls to ensure precision and minimal overhead.

Inspired by the legendary [bench.sh](https://github.com/teddysun/across/blob/master/bench.sh), rewritten in Modern C++ for maximum accuracy and zero-overhead performance.

## 🔥 Key Features

* **Hardcore Disk I/O Test**: Uses `O_DIRECT` flag to bypass RAM Cache (Page Cache), measuring true raw disk speed.
* **Detailed System Info**: Deep hardware detection (CPU Model, Cache, Virtualization Type, Swap Types) without relying on external tools.
* **Network Speedtest**: Integrates with official Ookla Speedtest CLI via JSON parsing for accurate latency, jitter, and packet loss data.
* **Memory Safe & Robust**: Built with RAII principles, Async-Signal-Safe handling, and optimistic error management.
* **Fully Static Binary**: Zero runtime dependencies - runs on any Linux distribution.
* **Modern Tech Stack**: Leverages C++23 features (`std::print`, `std::format`, `std::expected`).

---

## 📦 Quick Start (Pre-built Binary)

Download and run the pre-built static binary - **no compilation required**:

```bash
curl -L -o bench https://github.com/relvinarsenio/bench/releases/latest/download/bench \
  && chmod +x bench \
  && ./bench
```

---

## 🛠️ Build from Source

### Requirements

| Component | Minimum Version | Notes |
|-----------|-----------------|-------|
| **OS** | Linux | Any distro (Ubuntu, Debian, RHEL, Alpine, etc.) |
| **Compiler** | GCC 14+ or Clang 20+ | Auto-detected based on std::print support |
| **CMake** | 3.24+ | Build system |
| **Perl** | 5.x | Required for OpenSSL build |

### Smart Compiler Detection 🧠

The build system automatically detects the best compiler configuration:

| Priority | Compiler | Stdlib | When Used |
|----------|----------|--------|-----------|
| 1️⃣ | GCC 14+ | libstdc++ | If std::print works (Fedora 41+, RHEL 10+) |
| 2️⃣ | Clang 20+ | libc++ | Ubuntu/Alpine (libc++ has std::print) |
| 3️⃣ | Clang 20+ | libstdc++ | RHEL/Fedora with updated libstdc++ |

> **Note**: Ubuntu 24.04's libstdc++ lacks `<print>` support, so we use LLVM's libc++. RHEL/Fedora may have updated libstdc++ that works with Clang.

### All Dependencies Built from Source ✨

This project is **fully reproducible** - all dependencies are automatically downloaded and built from source during CMake configuration:

| Library | Version | Protocols/Features |
|---------|---------|-------------------|
| **zlib** | 1.3.1 | Compression |
| **OpenSSL** | 3.6.0 | TLS/SSL (libs only, no binaries) |
| **libcurl** | 8.17.0 | HTTP/HTTPS only (minimal build) |
| **nlohmann/json** | 3.12.0 | JSON parsing |

> **Note**: libcurl is built with **minimal protocols** (HTTP/HTTPS only). FTP, GOPHER, TELNET, etc. are disabled to reduce binary size.

### Option 1: Docker Build (Recommended) 🐳

The easiest way to build a fully static musl binary (~6.5MB):

```bash
# Clone the repo
git clone https://github.com/relvinarsenio/bench.git
cd bench

# Build with Docker
chmod +x build-static.sh
./build-static.sh

# Run
./dist/bench
```

Or manually with Docker:

```bash
docker build -t bench-builder .
docker create --name extract bench-builder
docker cp extract:/src/build/bench ./bench
docker rm extract
./bench
```

### Option 2: Native Build (Ubuntu/Debian)

#### Install Build Tools

```bash
# Install LLVM 20 (Ubuntu 24.04+)
sudo apt update
sudo apt install -y \
    cmake \
    clang-20 \
    lld-20 \
    libc++-20-dev \
    libc++abi-20-dev \
    perl \
    xxd 

# For older Ubuntu, add LLVM repo first:
# wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && sudo ./llvm.sh all 20
```

#### Build

```bash
# Configure (auto-detects compiler and stdlib)
CC=clang-20 CXX=clang++-20 cmake -DCMAKE_BUILD_TYPE="Release" -S . -B build

# Build
cmake --build build -j$(nproc)

# Strip and run (~7.4 MB with glibc)
strip build/bench
./build/bench
```

### Option 3: Native Build (RHEL/Fedora with GCC 14+)

```bash
# Install build tools (GCC 14+ and libstdc++ have std::print support)
sudo dnf install -y gcc-c++ cmake make perl xxd

# Build (auto-detects GCC + libstdc++)
cmake -DCMAKE_BUILD_TYPE="Release" -S . -B build
cmake --build build -j$(nproc)

# Strip and run
strip build/bench
./build/bench
```

### Option 4: Native Build (Alpine Linux)

```bash
# Install build tools only
apk add clang clang-extra-tools lld llvm libc++-static libc++-dev compiler-rt \
    cmake make linux-headers perl bash xxd

# Build (all libraries compiled from source)
CC=clang CXX=clang++ cmake -B build -DCMAKE_BUILD_TYPE=Release

cmake --build build -j$(nproc)

# The binary is fully static (~6.5 MB with musl)
strip build/bench
./build/bench
```

---

## 📁 Project Structure

```
bench/
├── CMakeLists.txt          # Main build configuration
├── Dockerfile              # Docker build for musl static binary
├── build-static.sh         # Helper script for Docker builds
├── cmake/
│   ├── DetectCompiler.cmake # Auto-detect GCC 14+ or Clang 20+
│   └── StaticDeps.cmake    # Builds ALL deps from source (zlib, openssl, curl, json)
├── include/                # Header files
│   ├── cli_renderer.hpp
│   ├── config.hpp
│   ├── disk_benchmark.hpp
│   ├── http_client.hpp
│   ├── speed_test.hpp
│   ├── system_info.hpp
│   └── ...
└── src/                    # Source files
    ├── app/main.cpp
    ├── core/
    ├── io/
    ├── net/
    ├── os/
    ├── system/
    └── ui/
```

### Build Process

1. **CMake Configure** (~3-4 min first time):
   - Detects compiler (GCC 14+ or Clang 20+) and stdlib
   - Downloads zlib, OpenSSL, libcurl, nlohmann/json
   - Builds OpenSSL libraries (no binaries, faster!)
   - Configures all dependencies with minimal features

2. **CMake Build** (~1 min):
   - Compiles zlib and libcurl
   - Compiles application code
   - Links everything statically

3. **Result**: Single static executable
   - **musl/Alpine**: ~6.5 MB (smallest)
   - **glibc/Ubuntu**: ~7.4 MB

---

## 📊 Example Output

```
------------------------------------------------------------------------------
 A Bench Script (C++ Edition v6.9.6)
 Usage : ./bench
------------------------------------------------------------------------------
 -> CPU & Hardware
 CPU Model            : AMD Ryzen 5 7535HS with Radeon Graphics
 CPU Cores            : 4 @ 3824.4 MHz
 CPU Cache            : 16 MB
 AES-NI               : ✓ Enabled
 VM-x/AMD-V           : ✗ Disabled

 -> System Info
 OS                   : Oracle Linux Server 10.1
 Arch                 : x86_64 (64 Bit)
 Kernel               : 6.12.0-106.55.4.2.el10uek.x86_64
 TCP CC               : bbr
 Virtualization       : Hyper-V
 System Uptime        : 0 days, 2 hour 22 min
 Load Average         : 0.35, 0.18, 0.13

 -> Storage & Memory
 Total Disk           : 60.2 GB (8.4 GB Used)
 Total Mem            : 2.5 GB (1.6 GB Used)
 Total Swap           : 3.2 GB (1004.3 MB Used)
   -> Partition        : 3.2 GB (1004.3 MB Used) (/dev/sda2)
   -> ZSwap            : Enabled

 -> Network
 IPv4/IPv6            : ✓ Online / ✗ Offline
 ISP                  : AS7713 PT TELKOM INDONESIA
 Location             : Bandar Lampung / ID
 Region               : Lampung
------------------------------------------------------------------------------
Running I/O Test (1GB File)...
 I/O Speed (Run #1) : 2197.1 MB/s
 I/O Speed (Run #2) : 2573.6 MB/s
 I/O Speed (Run #3) : 4554.3 MB/s
 I/O Speed (Average) : 3108.3 MB/s
------------------------------------------------------------------------------
Downloading Speedtest CLI...
 Node Name              Download          Upload            Latency     Loss    
 Speedtest.net (Auto)   76.66 Mbps        21.53 Mbps        23.88 ms    6.19 %  
 Singapore, SG          69.49 Mbps        22.32 Mbps        29.71 ms    0.00 %  
 Los Angeles, US        36.35 Mbps        28.20 Mbps        197.47 ms   0.00 %  
 Montreal, CA           42.75 Mbps        21.44 Mbps        273.46 ms   0.00 %  
 Paris, FR              72.95 Mbps        22.23 Mbps        184.56 ms   0.00 %  
 Amsterdam, NL          63.13 Mbps        20.63 Mbps        260.96 ms   0.00 %  
 Melbourne, AU          78.91 Mbps        21.80 Mbps        280.85 ms   0.00 %  
------------------------------------------------------------------------------
 Finished in        : 213 sec
```

---

## ⚖️ License

This project is licensed under the **Apache License 2.0**.

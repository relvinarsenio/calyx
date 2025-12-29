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

| Component | Requirement | Notes |
|-----------|-------------|-------|
| **OS** | Linux | Any distro with Docker support |
| **Docker** | 20.10+ | Required for building |

### All Dependencies Built from Source ✨

This project is **fully reproducible** - all dependencies are automatically downloaded and built from source during the Docker build:

| Library | Version | Purpose | Optimization |
|---------|---------|---------|--------------|
| **zlib** | 1.3.1 | Compression | Full LTO + -Oz |
| **LibreSSL** | 4.2.1 | TLS/SSL (libs only) | Full LTO + -Oz |
| **libcurl** | 8.17.0 | HTTP/HTTPS only | Full LTO + -Oz |
| **nlohmann/json** | 3.12.0 | JSON parsing | Header-only |

> **Build Optimizations**:
> - All libraries compiled with **Full LTO** and **-Oz** for maximum performance
> - Final binary compiled with **-Oz** for size optimization
> - libcurl built with **ultra-minimal features** (HTTP/HTTPS only, no FTP/LDAP/SMTP/etc.)
> - LibreSSL built without apps/tests/netcat
> - ICF (Identical Code Folding) enabled for the final binary

### Build with Docker 🐳

The Docker build creates a fully static musl binary (~2.6MB):

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

#### Build Options

```bash
# Normal build (uses cached Docker image if available)
./build-static.sh

# Fresh build (removes existing Docker image and rebuilds from scratch)
./build-static.sh --fresh-build

# Show help
./build-static.sh --help
```

#### Manual Docker Commands

If you prefer to run Docker commands manually:

```bash
docker build -t bench-builder .
docker create --name extract bench-builder
docker cp extract:/src/build/bench ./bench
docker rm extract
./bench
```

---

## 📁 Project Structure

```
bench/
├── CMakeLists.txt          # Main build configuration
├── Dockerfile              # Docker build for musl static binary
├── build-static.sh         # Helper script for Docker builds
├── cmake/
│   ├── DetectCompiler.cmake # Clang + libc++ detection
│   └── StaticDeps.cmake    # Builds ALL deps from source with Full LTO
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

1. **Docker Build** (~5-6 min first time):
   - Uses Alpine Linux with **Clang + libc++ (Full LLVM Stack)**
   - Downloads zlib, LibreSSL, libcurl, nlohmann/json
   - Builds all libraries with **Full LTO + -Oz** for maximum performance
   - Final binary compiled with **-Oz** for size optimization
   - ICF (Identical Code Folding) enabled for further size reduction
   - Compiles and links everything statically

2. **Result**: Single static executable (~2.6 MB with musl, stripped)

---

## 📊 Example Output

```
------------------------------------------------------------------------------
 A Bench Script (C++ Edition v7.0.0)
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

# Bench - Modern Linux Server Benchmark

**Bench** is a high-performance Linux server benchmarking tool written in **Modern C++ (C++23)**. It is designed to provide accurate, detailed, and memory-safe performance metrics.

Unlike traditional bash scripts, Bench parses kernel interfaces (`/proc`, `/sys`) directly and utilizes native *system calls* to ensure precision and minimal overhead.

Inspired by the legendary [bench.sh](https://github.com/teddysun/across/blob/master/bench.sh), rewritten in Modern C++ for maximum accuracy and zero-overhead performance

## 🔥 Key Features

* **Hardcore Disk I/O Test**: Utilizes the `O_DIRECT` flag to bypass the RAM Cache (Page Cache), measuring the true raw speed of the disk.
* **Detailed System Info**: Performs deep hardware detection (CPU Model, Cache, Virtualization Type, Swap Types) without relying on external tools like `lscpu`.
* **Network Speedtest**: Integrates with the official Ookla Speedtest CLI (via JSON parsing) to provide accurate latency, jitter, and packet loss data.
* **Memory Safe & Robust**: Built with RAII principles, *Async-Signal-Safe* handling, and optimistic error management to ensure stability.
* **Modern Tech Stack**: Leverages the latest C++23 features such as `std::print`, `std::format`, and `std::expected`.

## 🛠️ Requirements

Since this project utilizes the latest C++ standards, ensure your environment supports:

* **OS**: Linux (RHEL, Oracle Linux, Ubuntu, Debian, etc.).
* **Compiler**: 
    * **GCC 14+** (Native support for `<print>`).
    * **Clang 20+** (Support via LLVM Full Stack: `libc++` + `lld`).
* **Build System**: CMake 3.20+.
* **Dependencies**: `libcurl-devel` (RHEL/CentOS) or `libcurl4-openssl-dev` (Debian/Ubuntu).

## 🚀 Build & Install

### 1. Install Dependencies

**Ubuntu / Debian:**
```bash
sudo apt update
sudo apt install cmake build-essential libcurl4-openssl-dev
# Optional: Install LLVM Full Stack
# sudo apt install clang-20 libc++-20-dev libc++abi-20-dev lld-20
```

**RHEL / Oracle Linux:**
```bash
sudo dnf install cmake gcc-c++ libcurl-devel
```

### 2. Build Project

Bench features an intelligent build system that automatically detects the optimal configuration for your compiler.

#### Option A: Using GCC (Default)
If you have GCC 14+ installed, simply run:
```bash
cmake -DCMAKE_BUILD_TYPE="Release" -S . -B build
cmake --build build --parallel
```

#### Option B: Using LLVM / Clang (Recommended for Performance)
If you are using Clang, CMake will automatically enable **Thin LTO (Link Time Optimization)** and detect the appropriate standard library.

If your system's `libstdc++` is outdated (lacks `<print>` support), CMake will automatically switch to the **LLVM Full Stack** (`libc++` + `lld`).

```bash
# Example using Clang 20
cmake -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_BUILD_TYPE="Release" -S . -B build
cmake --build build --parallel
```

### 3. Run
```bash
./build/bench
```

## 📦 Run via pre-built binary (Recommended)

For those who prefer not to compile from source, you can download the pre-built binaries directly from the [Release Page](https://github.com/relvinarsenio/bench/releases/):

**All Distro:**
```bash
curl -L -o bench https://github.com/relvinarsenio/bench/releases/latest/download/bench && chmod +x bench && ./bench
```

## 📊 Example Output

```text
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
 Amsterdam, NL          Error: Cannot open socket
 Melbourne, AU          78.91 Mbps        21.80 Mbps        280.85 ms   0.00 %  
------------------------------------------------------------------------------
 Finished in        : 213 sec
```

## ⚖️ License

This project is licensed under the **Apache License 2.0**.

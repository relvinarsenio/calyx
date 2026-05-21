# 🧬 Calyx: Simple & Precise Linux System Benchmarking

[![License: MPL 2.0](https://img.shields.io/badge/License-MPL%202.0-brightgreen.svg)](https://opensource.org/licenses/MPL-2.0)
[![C++23 Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/compiler_support/23)
[![Static Binary](https://img.shields.io/badge/Build-Static--PIE-orange.svg)](#)

**Calyx** is a lightweight, all-in-one system benchmarking and diagnostic tool for Linux. It is designed as a high-performance, fully native C++23 alternative to classic scripts like `bench.sh` or `yabs.sh`.

If you've ever wondered *"What are my server's specs?"*, *"How much RAM is currently used?"*, *"How fast is my disk?"*, or *"What is my actual internet speed?"*, Calyx gives you the answers instantly. It runs directly in your terminal and produces a beautiful, easy-to-read hardware and network performance report.

---

## 🌟 What makes Calyx different?

Many benchmarking tools are either too complicated for beginners or too inaccurate because of system cache "cheats". Calyx solves both:

*   **Beginner-Friendly**: No installation, no complex arguments. Just run one command, and it shows you everything you need to know.
*   **Bypasses Cache "Cheating"**: When testing disk speeds, Linux normally caches files in RAM, making slow disks look artificially fast. Calyx uses modern Linux `io_uring` and `O_DIRECT` to bypass this, measuring your **real** hardware capability.
*   **Intelligent & Adaptive**: Calyx automatically tunes itself to your Linux kernel. It probes your system limits and configures the fastest possible path automatically.
*   **Ultra-Portable**: It compiles down to a single tiny file (~3 MB) that has no dependencies. You can copy it to any Linux machine and run it instantly.

---

## 📊 What does it measure?

When you run Calyx, it tests four main areas of your system:

### 1. 🧠 CPU & Hardware
*   Detects your CPU model name, active cores, and max clock frequency.
*   Checks if security hardware acceleration (**AES-NI**) is enabled.
*   Checks if Hardware Virtualization is supported.

### 2. 🐧 OS & System Information
*   Identifies your Linux distribution, kernel version, and virtualization type (e.g., Docker, KVM, Hyper-V).
*   Monitors uptime and system load averages.
*   Displays your current TCP Congestion Control algorithm (e.g., `bbr`, `cubic`).

### 3. 💾 Storage, Memory & ZSwap
*   Lists partition sizes, used space, and filesystem type.
*   Measures real RAM and Swap usage.
*   **Disk I/O Performance**: Measures sequential read and write speeds using Linux-native `io_uring` and `O_DIRECT` to ensure cache-bypass accuracy.
*   **ZSwap Metrics**: Shows compression ratio, and helps you identify memory pressure:
    *   *Spilled*: Data pushed to disk because compressed memory was full (high values mean you need more RAM!).
    *   *Rejected*: Data that failed to compress because the CPU was too busy.
    *   *Capped*: Data rejected because the ZSwap pool size limit was hit.

### 4. 🌐 Internet & Network Speed
*   Performs an instant online check and ISP auto-lookup.
*   Tests download speed, upload speed, latency (ping), and packet loss to multiple regional and international nodes.

---

## 🚀 Quick Start (No Install Required)

You don't need to install or compile anything to use Calyx. Just copy and paste this command into your Linux terminal:

```bash
bash <(curl -fsL https://calyx.pages.dev/run)
```

> [!NOTE]
> This command automatically detects your system architecture, downloads the secure static binary, runs the benchmark, displays your report, and cleans up afterwards leaving your system clean.

### 💡 Troubleshooting: "io_uring fixed buffers disabled"

If you see `Performance Hint: io_uring fixed buffers disabled (Memory limit), using fallback.` in your storage test:

This is a very common hint on VPS servers or non-root accounts. It means Calyx wanted to use locked memory buffers for zero-copy high-performance I/O, but your system's locked memory limit (`ulimit -l`) was too low. 

**How to get maximum performance:**
*   **Run as root** (using `sudo` or as root user), which automatically bypasses the memory lock limits.
*   **Increase the limit manually** by running `ulimit -l unlimited` in your terminal session before running the benchmark.

---

## 🛠️ Building from Source

If you prefer to compile Calyx yourself, you can build a fully static, portable binary using Docker.

### Requirements
*   **Operating System**: Linux (any distribution)
*   **Kernel**: Version `5.10` or newer (required for modern disk benchmarks)
*   **Docker**: Version `20.10` or newer (Version `23.0` or newer recommended)

### Build Steps
1.  Clone the repository:
    ```bash
    git clone https://github.com/relvinarsenio/calyx.git
    cd calyx
    ```
2.  Run the static build script:
    ```bash
    chmod +x build-static.sh
    ./build-static.sh
    ```

Once completed, your single portable binary will be generated at `./dist/calyx`. You can run it locally:
```bash
./dist/calyx
```

---

## 📊 Example Output

Here is what the terminal output looks like when Calyx finishes running:

```text
------------------- Calyx - Linux System Benchmarking Utility (v1.0.0) -------------------
 Author             : Alfie Ardinata (https://calyx.pages.dev/)
 GitHub             : https://github.com/relvinarsenio/calyx
 Usage              : ./calyx
 ---------------------------------------------------------------------------------------
  -> CPU & Hardware
  CPU Model            : AMD Ryzen 5 7535HS with Radeon Graphics
  CPU Cores            : 6 @ 4584.2 MHz (Max)
  CPU Cache            : 16 MB
  AES-NI               : ✓ Enabled
  Hardware Virt        : ✗ Disabled
 
  -> System Info
  OS                   : Debian GNU/Linux 13 (trixie)
  Arch                 : x86_64 (64 Bit)
  Kernel               : 6.19.6+deb14-amd64
  TCP CC               : bbr
  Virtualization       : Hyper-V
  System Uptime        : 14 hours, 38 mins
  Load Average         : 1.54, 1.06, 0.97
 
  -> Storage & Memory
  Test Path            : /home/user/Github/calyx (/dev/sda2 (xfs))
  Size Partition       : 63 GB (25 GB Used)
  Total Mem            : 6.9 GB (4.5 GB Used)
  Total Swap           : 5.2 GB (3.1 GB Used)
    -> Partition        : 4 GB (2.8 GB Used) (/dev/sdb)
    -> ZSwap            : 1.2 GB → 308.1 MB (3.88×) [zstd, limit: 1.4 GB (20%)]
                          Spilled: 10.8 GB  Rejected: 22.1 GB  Capped: 157.1 MB
 
  -> Network
  IPv4/IPv6            : ✓ Online / ✗ Offline
  ISP                  : AS13335 Cloudflare, Inc.
  Location             : Bandar Lampung / ID
  Region               : Lampung
 ---------------------------------------------------------------------------------------
 Running I/O Test (1 GB File)...
  I/O Speed (Run #1)   :  Write   3337.3 MB/s   Read   4500.9 MB/s
  I/O Speed (Run #2)   :  Write   3289.3 MB/s   Read   4523.6 MB/s
  I/O Speed (Run #3)   :  Write   3360.9 MB/s   Read   4524.8 MB/s
  I/O Speed (Average)  :  Write   3328.9 MB/s   Read   4516.4 MB/s
 ---------------------------------------------------------------------------------------
 Downloading Speedtest CLI...
  Node Name              Download          Upload            Latency     Loss    
  Speedtest.net (Auto)   138.91 Mbps       44.68 Mbps        19.87 ms    0.00 %  
  Singapore, SG          157.43 Mbps       43.07 Mbps        30.02 ms    0.00 %  
  Los Angeles, US        154.83 Mbps       16.60 Mbps        229.35 ms   0.00 %  
  Montreal, CA           145.71 Mbps       15.15 Mbps        267.46 ms   0.00 %  
  London, UK             147.87 Mbps       22.79 Mbps        212.89 ms   0.00 %  
  Amsterdam, NL          131.58 Mbps       16.16 Mbps        282.82 ms   0.00 %  
  Sydney, AU             144.35 Mbps       15.17 Mbps        235.15 ms   0.00 %  
 ---------------------------------------------------------------------------------------
  Finished in        : 3 min 47 sec
```

---

## ⚖️ License & Code of Conduct

*   This project is licensed under the **Mozilla Public License 2.0**.
*   Please note that this project is released with a [Code of Conduct](CODE_OF_CONDUCT.md). By participating, you agree to abide by its terms.

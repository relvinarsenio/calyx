# Calyx: Linux System Benchmark

[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B&logoColor=white)](https://en.cppreference.com/w/cpp/compiler_support/23)
[![Linux](https://img.shields.io/badge/Linux-x86_64%20|%20ARM64-1f6feb?logo=linux&logoColor=white)](https://github.com/relvinarsenio/calyx/releases/latest)
[![CI/CD](https://github.com/relvinarsenio/calyx/actions/workflows/build.yml/badge.svg)](https://github.com/relvinarsenio/calyx/actions/workflows/build.yml)
[![Latest_release](https://img.shields.io/github/v/tag/relvinarsenio/calyx?label=release)](https://github.com/relvinarsenio/calyx/releases/latest)

**Calyx** is a lightweight Linux system benchmark written in modern C++. No installation or configuration needed.


---

## Why Calyx?

It prints your hardware specs, disk speeds, and network stats in one run.

*   **Zero Install**: One command, one binary. No package managers, no dependencies.
*   **Real Disk Speeds**: Calyx uses `io_uring` and `O_DIRECT` to measure actual disk throughput.
*   **Self-Tuning**: It checks your kernel capabilities and system limits, then picks the most suitable setup automatically.
*   **Just a File**: The static binary is ~3 MB. Copy it to any Linux box and run it.

---

## What it measures

### 1. CPU & Hardware
*   CPU model, core count, and max clock speed.
*   AES-NI and hardware virtualization support.

### 2. OS & System Info
*   Distro, kernel version, and virtualization type (Docker, KVM, etc.).
*   Uptime and load averages.
*   Current TCP congestion control algorithm.

### 3. Storage, Memory & ZSwap
*   Partition sizes, usage, and filesystem type.
*   RAM and swap usage.
*   **Disk I/O**: Sequential read/write speeds.
*   **ZSwap**: Compression ratio and pressure stats:
    *   *Spilled*: Compressed data that was later written back to disk. High numbers mean memory pressure is pushing data out of zswap.
    *   *Rejected*: Data that zswap refused to compress because a reclaim attempt failed, usually under heavy memory pressure.
    *   *Capped*: Data that never entered zswap because the pool was already at its size limit.

### 4. Internet & Network
*   Online status and ISP lookup.
*   Download, upload, latency, and packet loss to multiple global nodes.

---

## Quick Start

No install needed. Paste this into your terminal:

```bash
bash <(curl -fsL https://calyx.pages.dev/run)
```

### Troubleshooting: "io_uring fixed buffers disabled"

> [!IMPORTANT]
> If you see a message about `io_uring fixed buffers disabled` in your output, Calyx tried to allocate locked memory for zero-copy I/O, but your `ulimit -l` is too low. The benchmark still runs fine, just not at maximum speed.

**To run at full speed:**
*   Run as root (`sudo`), which ignores the memory lock limit.
*   Or run `ulimit -l unlimited` before the benchmark.

---

## Building from Source

You can build a fully static binary with Docker.

> [!IMPORTANT]
> The `build-static.sh` script uses Docker for a consistent build.
> Manual host builds may fail or behave differently depending on the build environment.

### Requirements
*   **OS**: Linux
*   **Arch**: `x86_64` or `aarch64`
*   **Kernel**: 5.10 or newer
*   **Docker**: 20.10+ (23.0+ recommended)

### Steps
1.  Clone:
    ```bash
    git clone https://github.com/relvinarsenio/calyx.git
    cd calyx
    ```
2.  Build:
    ```bash
    chmod +x build-static.sh
    ./build-static.sh
    ```

The binary lands at `./dist/calyx`. Run it:
```bash
./dist/calyx
```

---

## Example Output

```
──────────────────────── Calyx - Linux System Benchmarking Utility (v1.2.0) ────────────────────────
 Author             : Alfie Ardinata (https://calyx.pages.dev/)
 GitHub             : https://github.com/relvinarsenio/calyx
 Usage              : ./calyx
────────────────────────────────────────────────────────────────────────────────────────────────────
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
  Disk Capacity        : 64 GB (/dev/sda)
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
────────────────────────────────────────────────────────────────────────────────────────────────────
Running I/O Test (1 GB File, Seq 1M Q16T1)...
 [ Throughput ]
   I/O Speed (Run #1)  :  Write    2.96 GB/s  │  Read    4.47 GB/s
   I/O Speed (Run #2)  :  Write    3.13 GB/s  │  Read    4.41 GB/s
   I/O Speed (Run #3)  :  Write    3.21 GB/s  │  Read    4.47 GB/s
   I/O Speed (Average) :  Write    3.11 GB/s  │  Read    4.45 GB/s

 [ Write Latency ]
   • Summary    :  Avg:    5.02 ms │ Min:    1.93 ms │ Max:    16.7 ms
   • Percentile :  p50:     4.2 ms │ p95:    9.51 ms │ p99:    10.6 ms │ p99.9:   16.59 ms

 [ Read Latency ]
   • Summary    :  Avg:    3.48 ms │ Min:    1.43 ms │ Max:    5.44 ms
   • Percentile :  p50:    3.49 ms │ p95:    3.96 ms │ p99:    4.47 ms │ p99.9:    5.25 ms
────────────────────────────────────────────────────────────────────────────────────────────────────
 Downloading Speedtest CLI...
  Node Name              Download          Upload            Latency     Loss    
  Speedtest.net (Auto)   138.91 Mbps       44.68 Mbps        19.87 ms    0.00 %  
  Singapore, SG          157.43 Mbps       43.07 Mbps        30.02 ms    0.00 %  
  Los Angeles, US        154.83 Mbps       16.60 Mbps        229.35 ms   0.00 %  
  Montreal, CA           145.71 Mbps       15.15 Mbps        267.46 ms   0.00 %  
  London, UK             147.87 Mbps       22.79 Mbps        212.89 ms   0.00 %  
  Amsterdam, NL          131.58 Mbps       16.16 Mbps        282.82 ms   0.00 %  
  Sydney, AU             144.35 Mbps       15.17 Mbps        235.15 ms   0.00 %  
 ────────────────────────────────────────────────────────────────────────────────────────────────────
  Finished in        : 3 min 47 sec
```

---

## Contributing & Support

We welcome contributions! Check out our [Contributing Guidelines](CONTRIBUTING.md) to get started.

Found a bug or have a feature request? Please [open an issue](https://github.com/relvinarsenio/calyx/issues) on our GitHub repository.

---

## License & Code of Conduct

*   This project is licensed under the **Mozilla Public License 2.0**.
*   Please note that this project is released with a [Code of Conduct](CODE_OF_CONDUCT.md). By participating, you agree to abide by its terms.

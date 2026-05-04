# Calyx - High-Precision Linux System Benchmarking

**Calyx** is a standalone system profiling and benchmarking utility designed for accuracy and reproducibility. Built with **Modern C++ (C++23)**, it utilizes Linux-native interfaces to provide deep insights into hardware and kernel performance with minimal overhead.

## ✨ Engineering Excellence

*   📊 **Statistical Stability Engine**: Not just an average; Calyx uses the **Coefficient of Variation (CV)** to weight multiple benchmark runs. This mathematical approach filters out transient system noise, ensuring your data reflects actual hardware capability.
*   🛡️ **Self-Optimizing I/O Pipeline**: Calyx doesn't just use `io_uring`; it intelligently probes your kernel to negotiate the most efficient I/O path available. Whether it's **Zero-Copy Fixed Buffers** or **SQPOLL threads**, Calyx automatically tunes its engine to your environment, ensuring you get raw hardware performance without manual tuning.
*   🔄 **Adaptive Resource Probing**: Calyx automatically scales its `io_uring` implementation. If system limits (`ulimit -l`) or memory constraints prevent fixed-buffer registration, it intelligently probes for the maximum possible count before falling back to standard paths.
*   🎯 **Deterministic Reproducibility**: Uses a fast **Xoshiro256++** PRNG with a fixed seed (digits of Pi) to ensure every I/O pattern is identical across different runs and machines.
*   🏗️ **Cache-Friendly Design**: The statistical engine is cache-line aligned and sized to fit within L1 Data Cache, minimizing cache-pollution during high-throughput testing.

## 🔥 Key Features

*   ⚡ **io_uring & O_DIRECT**: High-concurrency asynchronous I/O that bypasses the Linux Page Cache for raw disk throughput measurement.
*   🌐 **Overlapped Diagnostics**: Network probes and ISP metadata retrieval run in parallel with system checks to minimize execution time.
*   🧠 **Understanding ZSwap Metrics**: 
    *   **Spilled** (Written-back): Data forced to disk swap due to ZSwap overflow. High values indicate actual RAM exhaustion.
    *   **Rejected** (Reclaim Fail): Data that failed to enter ZSwap because the system couldn't cycle space fast enough.
    *   **Capped** (Pool Limit Hit): Data turned away because the configured max pool size was reached.
*   🏗️ **Fully Static & Portable**: A single ~3MB binary (Static PIE) with zero runtime dependencies. Runs on any kernel 5.10+ (x86_64 v3 or ARM64).

---

## 📦 Quick Start (Pre-built Binary)

Download and run the pre-built static binary - **no compilation required**:

```bash
curl -fsL https://calyx.pages.dev/run | bash
```

*(This script automatically detects your architecture, downloads the latest binary securely, runs the benchmark, and cleans up afterwards.)*

---

## 🛠️ Build from Source

### Requirements

| Component | Requirement | Notes |
| --- | --- | --- |
| **OS** | Linux | Any distro with Docker support |
| **Kernel** | **5.x+** | 5.10+ required for Disk Benchmark (`io_uring`) |
| **Docker** | 20.10+ | Required for hermetic static build |

### Dependencies (Hermetic Build)

This project is **fully reproducible**. All dependencies are built from source during the Docker build process:

* **zlib** (v1.3.2)
* **LibreSSL** (v4.3.1)
* **libcurl** (v8.20.0)
* **glaze** (v7.4.0)

### Build with Docker 🐳

The build script will create a fully static binary (~3 MB) inside the `dist/` folder:

```bash
# Clone the repo
git clone https://github.com/relvinarsenio/calyx.git
cd calyx

# Build using Docker
chmod +x build-static.sh
./build-static.sh
```

---

## 📊 Example Output

```text
------------------- Calyx - Modern Linux Performance Suite (v1.0.0) -------------------
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

## ⚖️ License

This project is licensed under the **Mozilla Public License 2.0**.

---

Please note that this project is released with a [Contributor Code of Conduct](CODE_OF_CONDUCT.md). By participating in this project you agree to abide by its terms.

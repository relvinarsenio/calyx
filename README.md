# Calyx - Rapid VPS Profiler

**Calyx** is a high-performance, rapid Linux server profiling and benchmarking tool written in **Modern C++ (C++23)**. It is designed to be completely static, memory-safe, and incredibly fast.

Unlike traditional bash scripts that rely on external tools (like `awk`, `sed`, or `grep`), Calyx parses kernel interfaces (`/proc`, `/sys`) directly using native C++ system calls for maximum precision and zero overhead.

## 🔥 Key Features

* **Hardcore Disk I/O Test**: Uses `O_DIRECT` + `io_uring` (where available) to bypass RAM Cache (Page Cache), measuring true raw disk speed / commit speed.
* **Rapid System Profiling**: Instant detection of CPU Model, Cache, Virtualization (Docker/KVM/Hyper-V), and specific RAM/Swap types (ZRAM/ZSwap).
* **Context-Aware Storage Check**: Automatically detects the filesystem and capacity of the specific partition where the test is running (supports OverlayFS, Btrfs, Ext4, etc.).
* **Network Speedtest**: Native integration with Ookla Speedtest CLI via JSON parsing for accurate Latency, Jitter, and Packet Loss data (impersonating a real browser to avoid blocks).
* **Fully Static Binary**: Zero runtime dependencies (Musl-linked) - runs on Linux Kernel 5.x+ with io_uring support distribution (Alpine, Ubuntu, CentOS, Arch, etc.).
* **Modern Tech Stack**: Built with C++23 (`std::print`, `std::expected`) and utilizes `io_uring` for asynchronous I/O.

---

## 📦 Quick Start (Pre-built Binary)

Download and run the pre-built static binary - **no compilation required**:

```bash
curl -fsL https://calyx.pages.dev/run | bash

```

*(This script automatically detects your architecture, downloads the latest binary securely to a temporary location, runs the benchmark, and cleans up afterwards.)*

---

## 🛠️ Build from Source

### Requirements

| Component | Requirement | Notes |
| --- | --- | --- |
| **OS** | Linux | Any distro with Docker support |
| **Kernel** | **5.x+** | 5.10+ required for Disk Benchmark (`io_uring`) |
| **Docker** | 20.10+ | Required for building static binary |

### Dependencies (Handled Automatically)

This project is **fully reproducible**. All dependencies are automatically downloaded and built from source during the Docker build process:

* **zlib** (v1.3.1) - Full LTO + -Oz
* **LibreSSL** (v4.2.1) - Full LTO + -Oz
* **libcurl** (v8.17.0) - Ultra-minimal (HTTP/HTTPS only)
* **nlohmann/json** (v3.12.0)

### Build with Docker 🐳

The build script will create a fully static binary (~2.7 MB) inside the `dist/` folder:

```bash
# Clone the repo
git clone https://github.com/relvinarsenio/calyx.git
cd calyx

# Build using Docker
chmod +x build-static.sh
./build-static.sh

# Run the result
./dist/calyx

```

#### Build Options

```bash
# Normal build (uses cached Docker image if available)
./build-static.sh

# Fresh build (force rebuild dependencies)
./build-static.sh --fresh-build

```

---

## 📊 Example Output

```text
--------------------- Calyx - Rapid VPS Profiler (v7.2.1) ----------------------
 Author             : Alfie Ardinata (https://calyx.pages.dev/)
 GitHub             : https://github.com/relvinarsenio/calyx
 Usage              : ./calyx
--------------------------------------------------------------------------------
 -> CPU & Hardware
 CPU Model            : AMD EPYC 7763 64-Core Processor
 CPU Cores            : 4 @ 2445.4 MHz
 CPU Cache            : 32 MB
 AES-NI               : ✓ Enabled
 VM-x/AMD-V           : ✓ Enabled

 -> System Info
 OS                   : Ubuntu 24.04.3 LTS
 Arch                 : x86_64 (64 Bit)
 Kernel               : 6.8.0-1030-azure
 TCP CC               : cubic
 Virtualization       : Docker
 System Uptime        : 3 hours, 6 mins
 Load Average         : 1.63, 0.82, 1.23

 -> Storage & Memory
 Disk Test Path       : /tmp (/dev/sda1 (ext4))
 Total Disk           : 117.6 GB (4.0 GB Used)
 Total Mem            : 15.6 GB (2.7 GB Used)
 Total Swap           : 3.2 GB (1.1 GB Used)
   -> Partition        : 3.2 GB (1.1 GB Used) (/dev/sda2)
   -> ZSwap            : Enabled

 -> Network
 IPv4/IPv6            : ✓ Online / ✗ Offline
 ISP                  : AS8075 Microsoft Corporation
 Location             : Singapore / SG
--------------------------------------------------------------------------------
Running I/O Test (1GB File)...
  I/O Speed (Run #1)   : Write    440.1 MB/s   Read    385.9 MB/s
  I/O Speed (Run #2)   : Write    390.3 MB/s   Read    392.7 MB/s
  I/O Speed (Run #3)   : Write    389.9 MB/s   Read    385.9 MB/s
  I/O Speed (Average)  : Write    406.8 MB/s   Read    388.2 MB/s
--------------------------------------------------------------------------------
Downloading Speedtest CLI...
 Node Name              Download          Upload            Latency     Loss    
 Speedtest.net (Auto)   27.94 Gbps        11.89 Gbps        1.12 ms     0.00 %  
 Singapore, SG          9.07 Gbps         5.33 Gbps         14.61 ms    0.00 %  
 Los Angeles, US        3.30 Gbps         389.92 Mbps       187.93 ms   0.00 %  
 Montreal, CA           3.40 Gbps         307.46 Mbps       224.41 ms   2.01 %  
 London, UK             5.98 Gbps         575.37 Mbps       155.56 ms   0.00 %  
 Amsterdam, NL          6.29 Gbps         647.44 Mbps       159.31 ms   0.00 %  
 Sydney, AU             7.57 Gbps         1.13 Gbps         94.01 ms    0.00 %  
--------------------------------------------------------------------------------
 Finished in        : 3 min 47 sec

```

---

## ⚖️ License

This project is licensed under the **Mozilla Public License 2.0**.

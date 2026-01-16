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
 CPU Model            : AMD Ryzen 5 7535HS with Radeon Graphics
 CPU Cores            : 4 @ 4587.8 MHz
 CPU Cache            : 16 MB
 AES-NI               : ✓ Enabled
 VM-x/AMD-V           : ✗ Disabled

 -> System Info
 OS                   : Oracle Linux Server 10.1
 Arch                 : x86_64 (64 Bit)
 Kernel               : 6.12.0-106.55.4.2.el10uek.x86_64
 TCP CC               : bbr
 Virtualization       : Hyper-V
 System Uptime        : 0 days, 4 hour 40 min
 Load Average         : 1.72, 0.87, 0.55

 -> Storage & Memory
 Disk Test Path       : /home/user/calyx (/dev/sda3 (btrfs))
 Total Disk           : 60.2 GB (12.1 GB Used)
 Total Mem            : 2.5 GB (1.6 GB Used)
 Total Swap           : 3.2 GB (1.1 GB Used)
   -> Partition        : 3.2 GB (1.1 GB Used) (/dev/sda2)
   -> ZSwap            : Enabled

 -> Network
 IPv4/IPv6            : ✓ Online / ✗ Offline
 ISP                  : AS7713 PT TELKOM INDONESIA
 Location             : Bandar Lampung / ID
 Region               : Lampung
--------------------------------------------------------------------------------
Running I/O Test (1GB File)...
  I/O Speed (Run #1)   : Write    952.2 MB/s   Read   4206.5 MB/s
  I/O Speed (Run #2)   : Write    194.4 MB/s   Read   4207.1 MB/s
  I/O Speed (Run #3)   : Write   1168.6 MB/s   Read   4195.1 MB/s
  I/O Speed (Average)  : Write    771.7 MB/s   Read   4202.9 MB/s
Note: Write speed reflects real disk commit speed, not temporary cache speed.
--------------------------------------------------------------------------------
Downloading Speedtest CLI...
 Node Name              Download          Upload            Latency     Loss    
 Speedtest.net (Auto)   36.74 Mbps        22.96 Mbps        21.55 ms    0.00 %  
 Singapore, SG          71.06 Mbps        21.83 Mbps        28.42 ms    0.00 %  
 Los Angeles, US        23.18 Mbps        21.72 Mbps        219.95 ms   0.42 %  
 Montreal, CA           39.62 Mbps        31.46 Mbps        291.89 ms   0.00 %  
 Paris, FR              54.28 Mbps        20.32 Mbps        194.75 ms   0.00 %  
 Amsterdam, NL          71.38 Mbps        20.90 Mbps        295.83 ms   0.00 %  
 Melbourne, AU          72.43 Mbps        20.49 Mbps        306.23 ms   0.00 %  
--------------------------------------------------------------------------------
 Finished in        : 2 min 55 sec

```

---

## ⚖️ License

This project is licensed under the **Mozilla Public License 2.0**.

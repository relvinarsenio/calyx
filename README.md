# Bench - Modern Linux Server Benchmark

**Bench** adalah tool benchmark server Linux yang ditulis menggunakan **Modern C++ (C++23)**. Tool ini dirancang untuk memberikan metrik performa yang akurat, detail, dan aman (memory-safe).

Tidak seperti script bash biasa, Bench melakukan parsing langsung ke kernel interface (`/proc`, `/sys`) dan menggunakan *system calls* native untuk hasil yang presisi.

## 🔥 Fitur Unggulan

* **Hardcore Disk I/O Test**: Menggunakan flag `O_DIRECT` untuk menembus RAM Cache (Page Cache) dan mengukur kecepatan disk yang sesungguhnya.
* **Detailed System Info**: Mendeteksi hardware secara mendalam (CPU Model, Cache, Virtualization Type, Swap Types) tanpa bergantung pada tool eksternal seperti `lscpu`.
* **Network Speedtest**: Integrasi dengan binary resmi Ookla Speedtest CLI (via JSON parsing) untuk data latensi, jitter, dan packet loss yang akurat.
* **Memory Safe & Robust**: Ditulis dengan prinsip RAII, *Async-Signal-Safe*, dan penanganan error yang "optimistic" (tidak mudah crash).
* **Modern Tech Stack**: Menggunakan fitur terbaru C++23 seperti `std::print`, `std::format`, dan `std::expected`.

## 🛠️ Requirements

Karena menggunakan standar C++ terbaru, pastikan environment kamu mendukung:

* **OS**: Linux (RHEL, Oracle Linux, Ubuntu, Debian, dll).
* **Compiler**: GCC 14+ atau Clang 20+ (Wajib support C++23 `<print>`).
* **Build System**: CMake 3.20+.
* **Dependencies**: `libcurl-devel` (RHEL/CentOS) atau `libcurl4-openssl-dev` (Debian/Ubuntu).

## 🚀 Cara Build & Install

1.  **Install Dependencies** (Contoh di Oracle Linux / RHEL):
    ```bash
    sudo dnf install cmake gcc-c++ libcurl-devel
    ```

2.  **Clone & Build**:
    ```bash
    git clone https://github.com/relvinarsenio/bench.git
    cd bench
    mkdir build && cd build
    cmake ..
    make
    ```
    *Note: CMake akan otomatis mendownload library `nlohmann/json`.*

3.  **Jalankan**:
    ```bash
    ./bench
    ```

## 📊 Contoh Output

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

Project ini dilisensikan di bawah **Apache License 2.0**.

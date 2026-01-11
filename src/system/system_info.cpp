/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/system_info.hpp"
#include "include/utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
#if !defined(__i386__) && !defined(__x86_64__)
static std::string cached_cpuinfo;
static std::mutex cpuinfo_mutex;
static bool cpuinfo_loaded = false;

const std::string& get_cached_cpuinfo() {
    std::lock_guard<std::mutex> lock(cpuinfo_mutex);
    if (!cpuinfo_loaded) {
        std::ifstream f("/proc/cpuinfo");
        if (f.is_open()) {
            std::ostringstream oss;
            oss << f.rdbuf();
            cached_cpuinfo = oss.str();
        }
        cpuinfo_loaded = true;
    }
    return cached_cpuinfo;
}
#endif

bool is_starts_with_ic(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size())
        return false;
    return std::equal(prefix.begin(), prefix.end(), str.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

std::string capitalize(std::string_view s) {
    if (s.empty())
        return {};
    std::string ret(s);
    ret[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ret[0])));
    if (ret == "Zram")
        return "ZRAM";
    return ret;
}

#if !defined(__i386__) && !defined(__x86_64__)
bool cpu_has_flag(std::string_view flag) {
    const auto& cpuinfo = get_cached_cpuinfo();
    if (cpuinfo.empty())
        return false;

    size_t field_pos = cpuinfo.find("\nflags");
    if (field_pos == std::string::npos) {
        field_pos = cpuinfo.find("\nFeatures");
    }

    if (field_pos == std::string::npos) {
        if (cpuinfo.starts_with("flags"))
            field_pos = 0;
        else if (cpuinfo.starts_with("Features"))
            field_pos = 0;
    }

    if (field_pos == std::string::npos)
        return false;

    size_t line_end = cpuinfo.find('\n', field_pos);
    if (line_end == std::string::npos)
        line_end = cpuinfo.length();

    std::string_view flags_line(cpuinfo.data() + field_pos, line_end - field_pos);
    
    size_t colon_pos = flags_line.find(':');
    if (colon_pos != std::string_view::npos) {
        flags_line = flags_line.substr(colon_pos + 1);
    }
    
    size_t pos = 0;
    while (pos < flags_line.size()) {
        while (pos < flags_line.size() && std::isspace(static_cast<unsigned char>(flags_line[pos]))) {
            ++pos;
        }
        if (pos >= flags_line.size()) break;
        
        size_t token_start = pos;
        while (pos < flags_line.size() && !std::isspace(static_cast<unsigned char>(flags_line[pos]))) {
            ++pos;
        }
        
        std::string_view token = flags_line.substr(token_start, pos - token_start);
        if (token == flag) {
            return true;
        }
    }
    
    return false;
}
#endif

}  // namespace

std::string SystemInfo::get_model_name() {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext >= 0x80000004) {
        std::array<unsigned int, 12> data{};
        __cpuid(0x80000002, data[0], data[1], data[2], data[3]);
        __cpuid(0x80000003, data[4], data[5], data[6], data[7]);
        __cpuid(0x80000004, data[8], data[9], data[10], data[11]);

        std::string brand;
        brand.resize(48);
        std::memcpy(brand.data(), data.data(), std::min(brand.size(), sizeof(data)));

        if (auto pos = brand.find('\0'); pos != std::string::npos) {
            brand.resize(pos);
        }

        brand = trim(brand);
        if (!brand.empty())
            return brand;
    }
#endif

    std::ifstream f("/proc/cpuinfo");
    std::string line;
    const std::array<std::string, 5> keys = {"model name", "hardware", "processor", "cpu", "Model"};

    while (std::getline(f, line)) {
        for (const auto& k : keys) {
            if (is_starts_with_ic(line, k)) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    auto model = trim(line.substr(colon + 1));
                    if (!model.empty())
                        return model;
                }
            }
        }
    }

    std::ifstream dt("/sys/firmware/devicetree/base/model");
    if (dt) {
        std::string model;
        if (std::getline(dt, model)) {
            model = trim(model);
            if (!model.empty())
                return model;
        }
    }

    struct utsname buf {};
    if (::uname(&buf) == 0)
        return std::string(buf.machine);
    return "Unknown CPU";
}

std::string SystemInfo::get_cpu_cores_freq() {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    double freq_mhz = 0.0;

    std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");

    std::string line;
    if (f >> line) {
        uint64_t val = 0;
        auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), val);
        if (ec == std::errc()) {
            freq_mhz = static_cast<double>(val) / 1000.0;
        }
    }

    if (freq_mhz == 0.0) {
        std::ifstream cpuinfo("/proc/cpuinfo");
        while (std::getline(cpuinfo, line)) {
            if (line.starts_with("cpu MHz")) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string_view val_str = trim_sv(std::string_view(line).substr(colon + 1));
                    auto [ptr, ec] =
                        std::from_chars(val_str.data(), val_str.data() + val_str.size(), freq_mhz);
                    if (ec == std::errc()) {
                        break;
                    }
                }
            }
        }
    }
    return std::format("{} @ {:.1f} MHz", cores, freq_mhz);
}

std::string SystemInfo::get_cpu_cache() {
    auto parse_cache = [](std::string s) -> std::string {
        s = trim(s);
        if (s.empty())
            return "Unknown";

        uint64_t size = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), size);

        if (ec != std::errc())
            return s;

        if (ptr < s.data() + s.size()) {
            char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(*ptr)));
            if (suffix == 'K')
                size *= 1024;
            else if (suffix == 'M')
                size *= 1024 * 1024;
        } else {
            if (static_cast<char>(std::toupper(static_cast<unsigned char>(s.back()))) == 'K')
                size *= 1024;
            else if (std::isdigit(static_cast<unsigned char>(s.back())))
                size *= 1024;
        }

        if (size >= 1024 * 1024)
            return std::format("{:.0f} MB", static_cast<double>(size) / (1024.0 * 1024.0));
        if (size >= 1024)
            return std::format("{:.0f} KB", static_cast<double>(size) / 1024.0);
        return std::format("{} B", size);
    };

    constexpr std::array<std::string_view, 4> caches = {"3", "2", "1", "0"};
    for (const auto& idx : caches) {
        std::string path = std::format("/sys/devices/system/cpu/cpu0/cache/index{}/size", idx);
        std::ifstream f(path);
        std::string size;
        if (f >> size)
            return parse_cache(size);
    }
    return "Unknown";
}

bool SystemInfo::has_aes() {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);
    return (ecx & (1U << 25)) != 0;  // AES-NI: ECX bit 25
#else
    return cpu_has_flag("aes");
#endif
}

bool SystemInfo::has_vmx() {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);

    // Intel VMX: ECX bit 5
    bool intel_vmx = (ecx & (1U << 5)) != 0;

    // AMD SVM: CPUID leaf 0x80000001, ECX bit 2
    bool amd_svm = false;
    unsigned int max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext >= 0x80000001) {
        __cpuid(0x80000001, eax, ebx, ecx, edx);
        amd_svm = (ecx & (1U << 2)) != 0;
    }

    return intel_vmx || amd_svm;
#else
    return cpu_has_flag("vmx") || cpu_has_flag("svm");
#endif
}

std::string SystemInfo::get_virtualization() {
    if (fs::exists("/.dockerenv") || fs::exists("/run/.containerenv"))
        return "Docker";

    if (fs::exists("/proc/1/environ")) {
        std::ifstream f("/proc/1/environ");
        std::string env;
        while (std::getline(f, env, '\0')) {
            if (env.find("container=lxc") != std::string::npos)
                return "LXC";
        }
    }

    if (fs::exists("/proc/user_beancounters"))
        return "OpenVZ";

    struct utsname buffer;
    if (uname(&buffer) == 0) {
        std::string release = buffer.release;
        if (release.find("Microsoft") != std::string::npos ||
            release.find("WSL") != std::string::npos)
            return "WSL";
    }

    if (fs::exists("/proc/1/environ")) {
        std::ifstream f("/proc/1/environ");
        std::string env;
        while (std::getline(f, env, '\0')) {
            if (env.find("WSL_DISTRO_NAME=") != std::string::npos ||
                env.find("WSL_INTEROP=") != std::string::npos ||
                env.find("WSLENV=") != std::string::npos) {
                return "WSL";
            }
        }
    }

    if (fs::exists("/dev/dxg")) {
        return "WSL";
    }
    if (fs::exists("/dev/lxss")) {
        return "WSL";
    }

    if (fs::exists("/usr/lib/wsl") || fs::exists("/mnt/wsl")) {
        return "WSL";
    }

    bool hv_bit = false;
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);
    if (ecx & (1U << 31))
        hv_bit = true;
#endif

    if (hv_bit) {
#if defined(__x86_64__) || defined(__i386__)
        unsigned int leaf = 0x40000000;
        __cpuid(leaf, eax, ebx, ecx, edx);

        std::array<char, 13> vendor{};
        std::memcpy(vendor.data(), &ebx, 4);
        std::memcpy(vendor.data() + 4, &ecx, 4);
        std::memcpy(vendor.data() + 8, &edx, 4);
        vendor[12] = '\0';

        std::string sig(vendor.data());

        if (sig == "KVMKVMKVM")
            return "KVM";
        if (sig == "Microsoft Hv")
            return "Hyper-V";
        if (sig == "VMwareVMware")
            return "VMware";
        if (sig == "XenVMMXenVMM")
            return "Xen";
        if (sig == "VBoxVBoxVBox")
            return "VirtualBox";
        if (sig == "prl hyperv  ")
            return "Parallels";
        if (sig == "TCGTCGTCGTCG")
            return "QEMU";
#endif
    }

    std::ifstream dmi("/sys/class/dmi/id/product_name");
    std::string product;
    if (std::getline(dmi, product)) {
        if (product.find("KVM") != std::string::npos)
            return "KVM";
        if (product.find("QEMU") != std::string::npos)
            return "QEMU";
        if (product.find("VirtualBox") != std::string::npos)
            return "VirtualBox";
    }

    return hv_bit ? "Dedicated (Virtual)" : "Dedicated";
}

std::string SystemInfo::get_os() {
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("PRETTY_NAME=")) {
            auto val = line.substr(12);
            if (val.size() >= 2 && val.front() == '"')
                val = val.substr(1, val.size() - 2);
            return val;
        }
    }
    return "Linux";
}

std::string SystemInfo::get_arch() {
    struct utsname buffer;
    if (uname(&buffer) == 0) {
        std::string arch = buffer.machine;
        int bits = static_cast<int>(sizeof(void*) * 8);
        return std::format("{} ({} Bit)", arch, bits);
    }
    return "Unknown";
}

std::string SystemInfo::get_kernel() {
    struct utsname buffer;
    if (uname(&buffer) == 0)
        return buffer.release;
    return "Unknown";
}

std::string SystemInfo::get_tcp_cc() {
    std::ifstream f("/proc/sys/net/ipv4/tcp_congestion_control");
    std::string s;
    if (f >> s)
        return s;
    return "Unknown";
}

std::string SystemInfo::get_uptime() {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        auto up = si.uptime;
        int days = static_cast<int>(up / 86400);
        int hours = static_cast<int>((up % 86400) / 3600);
        int mins = static_cast<int>((up % 3600) / 60);
        return std::format("{} days, {} hour {} min", days, hours, mins);
    }
    return "Unknown";
}

std::string SystemInfo::get_load_avg() {
    double loads[3];
    if (getloadavg(loads, 3) != -1) {
        return std::format("{:.2f}, {:.2f}, {:.2f}", loads[0], loads[1], loads[2]);
    }
    return "Unknown";
}

std::vector<SwapEntry> SystemInfo::get_swaps() {
    std::vector<SwapEntry> swaps;

    std::ifstream f("/proc/swaps");
    std::string line;

    if (std::getline(f, line)) {
        while (std::getline(f, line)) {
            std::stringstream ss(line);
            std::string path, type, size_str, used_str;

            if (ss >> path >> type >> size_str >> used_str) {
                SwapEntry entry;
                entry.path = path;

                if (path.find("zram") != std::string::npos) {
                    entry.type = "ZRAM";
                } else {
                    entry.type = capitalize(type);
                }

                uint64_t val_size = 0, val_used = 0;
                auto [p1, ec1] =
                    std::from_chars(size_str.data(), size_str.data() + size_str.size(), val_size);
                auto [p2, ec2] =
                    std::from_chars(used_str.data(), used_str.data() + used_str.size(), val_used);

                if (ec1 == std::errc() && ec2 == std::errc()) {
                    entry.size = val_size * 1024;
                    entry.used = val_used * 1024;
                    swaps.push_back(entry);
                }
            }
        }
    }

    std::ifstream z("/sys/module/zswap/parameters/enabled");
    char c;
    if (z >> c && (c == 'Y' || c == 'y' || c == '1')) {
        SwapEntry zswap;
        zswap.type = "ZSwap";
        zswap.path = "Enabled";
        zswap.size = 0;
        zswap.used = 0;
        zswap.is_zswap = true;
        swaps.push_back(zswap);
    }

    return swaps;
}

MemInfo SystemInfo::get_memory_status() {
    MemInfo info{};
    struct sysinfo si;

    if (sysinfo(&si) == 0) {
        info.total = si.totalram * si.mem_unit;
    }

    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    uint64_t mem_available = 0;

    while (std::getline(meminfo, line)) {
        if (line.starts_with("MemAvailable:")) {
            std::string_view sv = line;
            auto colon = sv.find(':');
            if (colon != std::string_view::npos) {
                sv = sv.substr(colon + 1);
                sv = trim_sv(sv);

                auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), mem_available);
                if (ec == std::errc()) {
                    mem_available *= 1024;
                } else {
                    mem_available = 0;
                }
                break;
            }
        }
    }

    if (mem_available > 0) {
        info.available = mem_available;
    } else {
        info.available = si.freeram * si.mem_unit;
    }

    if (info.total >= info.available) {
        info.used = info.total - info.available;
    } else {
        info.used = 0;
    }

    return info;
}

DiskInfo SystemInfo::get_disk_usage(const std::string& mountpoint) {
    DiskInfo info{};
    struct statvfs disk;

    if (statvfs(mountpoint.c_str(), &disk) == 0) {
        info.total = disk.f_blocks * disk.f_frsize;
        info.free = disk.f_bfree * disk.f_frsize;
        info.used = (disk.f_blocks - disk.f_bfree) * disk.f_frsize;
    }

    return info;
}

std::string SystemInfo::get_device_name(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0)
        return "unknown device";

    std::ifstream mountinfo("/proc/self/mountinfo");
    if (!mountinfo)
        return "unknown device";

    const std::string target_dev = std::format("{}:{}", major(st.st_dev), minor(st.st_dev));

    std::string best_path_match = "unknown device";
    size_t best_path_len = 0;

    std::string exact_dev_match;

    std::string line;
    while (std::getline(mountinfo, line)) {
        std::string_view line_view(line);
        std::vector<std::string_view> tokens;

        size_t start = 0;
        while (start < line_view.length()) {
            while (start < line_view.length() &&
                   (line_view[start] == ' ' || line_view[start] == '\t'))
                start++;
            if (start >= line_view.length())
                break;

            size_t end = start;
            while (end < line_view.length() && line_view[end] != ' ' && line_view[end] != '\t')
                end++;

            tokens.push_back(line_view.substr(start, end - start));
            start = end;
        }

        if (tokens.size() < 5)
            continue;

        std::string id(tokens[0]), parent(tokens[1]), major_minor(tokens[2]), root(tokens[3]),
            mount_point(tokens[4]);

        size_t dash_pos = 5;
        while (dash_pos < tokens.size() && tokens[dash_pos] != "-")
            dash_pos++;

        if (dash_pos + 2 >= tokens.size())
            continue;

        std::string fs_type(tokens[dash_pos + 1]), source(tokens[dash_pos + 2]);

        auto format_output = [&](const std::string& src, const std::string& fs) {
            if (src == fs)
                return src;
            return std::format("{} ({})", src, fs);
        };

        if (major_minor == target_dev) {
            exact_dev_match = format_output(source, fs_type);
        }

        if (path.compare(0, mount_point.size(), mount_point) == 0) {
            bool valid_boundary = path.size() == mount_point.size() || mount_point == "/" ||
                                  path[mount_point.size()] == '/';

            if (valid_boundary && mount_point.size() > best_path_len) {
                best_path_len = mount_point.size();
                best_path_match = format_output(source, fs_type);
            }
        }
    }

    if (best_path_len > 0 && best_path_match != "unknown device") {
        return best_path_match;
    }

    if (!exact_dev_match.empty()) {
        return exact_dev_match;
    }

    return best_path_match;
}
#include "include/system_info.hpp"
#include "include/utils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <string_view>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
    bool is_starts_with_ic(std::string_view str, std::string_view prefix) {
        if (str.size() < prefix.size()) return false;
        return std::equal(prefix.begin(), prefix.end(), str.begin(), 
            [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == 
                       std::tolower(static_cast<unsigned char>(b));
            });
    }

    std::string capitalize(std::string_view s) {
        if (s.empty()) return {};
        std::string ret(s);
        ret[0] = static_cast<char>(std::toupper(ret[0]));
        if (ret == "Zram") return "ZRAM";
        return ret;
    }
}

const std::string& SystemInfo::get_cpuinfo_cache() {
    static const std::string cache = []{
        std::ifstream f("/proc/cpuinfo");
        std::stringstream buffer;
        buffer << f.rdbuf();
        return buffer.str();
    }();
    return cache;
}

std::string SystemInfo::get_model_name() {
    #if defined(__i386__) || defined(__x86_64__)
        unsigned int max_ext = __get_cpuid_max(0x80000000, nullptr);
        if (max_ext >= 0x80000004) {
            std::array<unsigned int, 12> data{};
            __cpuid(0x80000002, data[0], data[1], data[2], data[3]);
            __cpuid(0x80000003, data[4], data[5], data[6], data[7]);
            __cpuid(0x80000004, data[8], data[9], data[10], data[11]);
            std::string brand(reinterpret_cast<char*>(data.data()), 48);
            brand = trim(brand);
            if (!brand.empty()) return brand;
        }
    #endif

    std::stringstream ss(get_cpuinfo_cache());
    std::string line;
    const std::array<std::string, 5> keys = {"model name", "hardware", "processor", "cpu", "Model"};
    
    while (std::getline(ss, line)) {
        for (const auto& k : keys) {
            if (is_starts_with_ic(line, k)) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    auto model = trim(line.substr(colon + 1));
                    if (!model.empty()) return model;
                }
            }
        }
    }

    std::ifstream dt("/sys/firmware/devicetree/base/model");
    if (dt) {
        std::string model;
        if (std::getline(dt, model)) {
            model = trim(model);
            if (!model.empty()) return model;
        }
    }

    struct utsname buf{};
    if (::uname(&buf) == 0) return std::string(buf.machine);
    return "Unknown CPU";
}

std::string SystemInfo::get_cpu_cores_freq() {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    double freq_mhz = 0.0;

    std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (f >> freq_mhz) {
        freq_mhz /= 1000.0;
    } else {
        std::stringstream ss(get_cpuinfo_cache());
        std::string line;
        while(std::getline(ss, line)) {
            if (line.starts_with("cpu MHz")) {
                try {
                    freq_mhz = std::stod(line.substr(line.find(':') + 1));
                    break;
                } catch (...) {}
            }
        }
    }
    return std::format("{} @ {:.1f} MHz", cores, freq_mhz);
}

std::string SystemInfo::get_cpu_cache() {
    auto parse_cache = [](std::string s) -> std::string {
        s = trim(s);
        if (s.empty()) return "Unknown";
        uint64_t size = 0;
        try {
            size_t idx;
            size = std::stoull(s, &idx);
            if (idx < s.size()) {
                char suffix = std::toupper(static_cast<unsigned char>(s[idx]));
                if (suffix == 'K') size *= 1024;
                else if (suffix == 'M') size *= 1024 * 1024;
            } else {
                if (s.back() != 'B') size *= 1024;
            }
        } catch (...) { return s; }

        if (size >= 1024 * 1024) return std::format("{:.0f} MB", size / (1024.0 * 1024.0));
        if (size >= 1024) return std::format("{:.0f} KB", size / 1024.0);
        return std::format("{} B", size);
    };

    std::vector<std::string> caches = {"3", "2", "1", "0"};
    for(const auto& idx : caches) {
        std::string path = "/sys/devices/system/cpu/cpu0/cache/index" + idx + "/size";
        std::ifstream f(path);
        std::string size;
        if (f >> size) return parse_cache(size);
    }
    return "Unknown";
}

bool SystemInfo::has_aes() {
    return get_cpuinfo_cache().find("aes") != std::string::npos;
}

bool SystemInfo::has_vmx() {
    const auto& content = get_cpuinfo_cache();
    return content.find("vmx") != std::string::npos || content.find("svm") != std::string::npos;
}

std::string SystemInfo::get_virtualization() {
    if (fs::exists("/.dockerenv") || fs::exists("/run/.containerenv")) return "Docker";
    
    if (fs::exists("/proc/1/environ")) {
        std::ifstream f("/proc/1/environ");
        std::string env;
        while (std::getline(f, env, '\0')) {
            if (env.find("container=lxc") != std::string::npos) return "LXC";
        }
    }
    
    if (fs::exists("/proc/user_beancounters")) return "OpenVZ";

    auto get_cpuid_vendor = [](unsigned int leaf) -> std::string {
        #if defined(__x86_64__) || defined(__i386__)
        unsigned int eax, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf));
        std::array<char, 13> vendor;
        auto copy_reg = [&](unsigned int reg, size_t offset) {
            auto bytes = std::bit_cast<std::array<char, 4>>(reg);
            std::copy(bytes.begin(), bytes.end(), vendor.begin() + offset);
        };
        copy_reg(ebx, 0); copy_reg(ecx, 4); copy_reg(edx, 8);
        vendor[12] = '\0';
        return std::string(vendor.data());
        #else
        return "";
        #endif
    };

    struct utsname buffer;
    if (uname(&buffer) == 0) {
        std::string release = buffer.release;
        if (release.find("Microsoft") != std::string::npos || release.find("WSL") != std::string::npos)
            return "WSL";
    }

    bool hv_bit = false;
    #if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (ecx & (1 << 31)) hv_bit = true;
    #endif

    if (hv_bit) {
        std::string sig = get_cpuid_vendor(0x40000000);
        if (sig == "KVMKVMKVM") return "KVM";
        if (sig == "Microsoft Hv") return "Hyper-V";
        if (sig == "VMwareVMware") return "VMware";
        if (sig == "XenVMMXenVMM") return "Xen";
        if (sig == "VBoxVBoxVBox") return "VirtualBox";
        if (sig == "prl hyperv  ") return "Parallels";
        if (sig == "TCGTCGTCGTCG") return "QEMU";
    }

    std::ifstream dmi("/sys/class/dmi/id/product_name");
    std::string product;
    if (std::getline(dmi, product)) {
        if (product.find("KVM") != std::string::npos) return "KVM";
        if (product.find("QEMU") != std::string::npos) return "QEMU";
        if (product.find("VirtualBox") != std::string::npos) return "VirtualBox";
    }

    return hv_bit ? "Dedicated (Virtual)" : "Dedicated";
}

std::string SystemInfo::get_os() {
    std::ifstream f("/etc/os-release");
    std::string line;
    while(std::getline(f, line)) {
        if (line.starts_with("PRETTY_NAME=")) {
            auto val = line.substr(12);
            if (val.size() >= 2 && val.front() == '"') val = val.substr(1, val.size()-2);
            return val;
        }
    }
    return "Linux";
}

std::string SystemInfo::get_arch() {
    struct utsname buffer;
    if (uname(&buffer) == 0) {
        std::string arch = buffer.machine;
        int bits = sizeof(void*) * 8;
        return std::format("{} ({} Bit)", arch, bits);
    }
    return "Unknown";
}

std::string SystemInfo::get_kernel() {
    struct utsname buffer;
    if (uname(&buffer) == 0) return buffer.release;
    return "Unknown";
}

std::string SystemInfo::get_tcp_cc() {
    std::ifstream f("/proc/sys/net/ipv4/tcp_congestion_control");
    std::string s;
    if(f >> s) return s;
    return "Unknown";
}

std::string SystemInfo::get_uptime() {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        long up = si.uptime;
        int days = up / 86400;
        int hours = (up % 86400) / 3600;
        int mins = (up % 3600) / 60;
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

                try {
                    entry.size = std::stoull(size_str) * 1024;
                    entry.used = std::stoull(used_str) * 1024;
                } catch (...) {
                    entry.size = 0;
                    entry.used = 0;
                }
                
                swaps.push_back(entry);
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
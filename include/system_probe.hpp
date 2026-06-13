/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "posix.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

namespace probe {

/**
 * @brief Zero-overhead, shared system probing results.
 * @details These constants use C++17 inline semantics to ensure single initialization
 * across all translation units, eliminating redundant I/O during startup.
 */

struct FreqInfo {
    std::uint64_t khz { 0 };
    bool is_true_max { false };
};
struct ArchInfo {
    std::string raw;
    std::string formatted;
};

inline const auto kUnameProbe { posix::uname() };

inline const ArchInfo kArchProbe = []() -> ArchInfo {
    return kUnameProbe
        .transform([](auto res) -> ArchInfo {
            const std::string raw { res.machine };
            const auto bits = (raw.contains("64") || raw == "s390x") ? 64
                : (raw.contains("86") || raw.starts_with("arm"))     ? 32
                                                                 : toUByte(std::numeric_limits<std::uintptr_t>::digits);
            return { raw, std::format("{} ({} Bit)", raw, bits) };
        })
        .value_or(ArchInfo { "unknown", "Unknown" });
}();

inline const std::string kCpuInfoProbe { read_file("/proc/cpuinfo").value_or(std::string {}) };

inline const std::string kOsReleaseProbe { read_file("/etc/os-release").value_or(std::string {}) };

inline const std::string kTcpCcProbe { parse_file_or<std::string>(
    "/proc/sys/net/ipv4/tcp_congestion_control", [](auto s) { return std::string { trim_sv(s) }; }, std::string {}) };

inline const std::string kCpuCacheProbe = []() -> std::string {
    auto parse_size = [](std::string_view sv) -> std::string {
        sv = trim_sv(sv);
        if (sv.empty()) { return "Unknown"; }
        std::uint64_t val { 0 };
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
        if (ec != std::errc {}) { return std::string { sv }; }
        const auto offset { toSize(std::distance(sv.data(), ptr)) };
        const auto rem { sv.substr(offset) };
        const auto p { rem.find_first_not_of(" \t") };
        const char s { (p != std::string_view::npos) ? toChar(std::toupper(toInt(toUChar(rem[p])))) : 'K' };
        static constexpr auto kUnits = std::to_array<std::pair<char, std::uint64_t>>(
            { { 'G', 1024ULL * 1024 * 1024 }, { 'M', 1024ULL * 1024 }, { 'K', 1024ULL }, { 'B', 1ULL } });
        auto unit_it                   = std::ranges::find_if(kUnits, [s](auto p) { return p.first == s; });
        const std::uint64_t multiplier = (unit_it != kUnits.end()) ? unit_it->second
            : (p == std::string_view::npos)                        ? 1024ULL
                                                                   : 1ULL;
        return format_bytes(multiplier * val);
    };

    struct CacheEntry {
        std::uint32_t level {};
        bool is_data {};
        std::string size;
    };

    /** @brief Scan all sysfs cache indices; index number != cache level on many architectures. */
    namespace fs = std::filesystem;
    static constexpr std::string_view kCacheDir { "/sys/devices/system/cpu/cpu0/cache" };
    std::optional<CacheEntry> best;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(kCacheDir, ec)) {
        if (!entry.is_directory(ec)) { continue; }
        const auto name = entry.path().filename().string();
        if (!name.starts_with("index")) { continue; }

        auto size_str = read_file(entry.path() / "size");
        if (!size_str) { continue; }

        const auto level_val = parse_file_or<std::uint32_t>(
            entry.path() / "level", [](auto s) { return parse_number<std::uint32_t>(trim_sv(s)).value_or(0U); }, 0U);
        const auto type_str = parse_file_or<std::string>(
            entry.path() / "type", [](auto s) { return std::string { trim_sv(s) }; }, std::string {});

        const bool data = (type_str == "Unified" || type_str == "Data");
        if (!best || level_val > best->level || (level_val == best->level && data && !best->is_data)) {
            best = CacheEntry { level_val, data, std::move(*size_str) };
        }
    }

    return best ? parse_size(best->size) : std::string { "Unknown" };
}();

inline const std::string kDtModelProbe { parse_file_or<std::string>(
    "/sys/firmware/devicetree/base/model",
    [](auto s) {
        const auto sv { trim_sv(s) };
        return std::string { sv.substr(0, sv.find('\0')) };
    },
    std::string {}) };

inline const std::string kMidrProbe { parse_file_or<std::string>(
    "/sys/devices/system/cpu/cpu0/regs/identification/midr_el1", [](auto s) { return std::string { trim_sv(s) }; },
    std::string {}) };

inline const bool kZswapEnabledProbe { parse_file_or<bool>(
    "/sys/module/zswap/parameters/enabled",
    [](auto s) {
        const auto t { trim_sv(s) };
        return t == "Y" || t == "y" || t == "1";
    },
    false) };

inline const FreqInfo kMaxFreqProbe = []() -> FreqInfo {
#if defined(__i386__) || defined(__x86_64__)
    if (__get_cpuid_max(0, nullptr) >= 0x16) {
        std::uint32_t eax {};
        std::uint32_t ebx {};
        std::uint32_t ecx {};
        std::uint32_t edx {};
        __cpuid(0x16, eax, ebx, ecx, edx);
        if (std::uint32_t m { ebx & std::uint32_t { 0xFFFF } }; m > std::uint32_t { 0 })
            return { toULong(m) * 1000, true };
    }
#endif

    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists("/sys/devices/system/cpu", ec)) {
        auto read_core = [](const fs::directory_entry& e) -> std::optional<FreqInfo> {
            std::error_code dir_ec;
            if (!e.is_directory(dir_ec) || dir_ec) return std::nullopt;
            const auto fname = e.path().filename().string();
            if (!fname.starts_with("cpu")) return std::nullopt;
            const std::string_view suffix { std::string_view { fname }.substr(3) };
            if (suffix.empty()
                || !std::ranges::all_of(suffix, [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                return std::nullopt;
            }
            const auto max_f { parse_file_or<std::uint64_t>(
                e.path() / "cpufreq/cpuinfo_max_freq",
                [](auto s) { return parse_number<std::uint64_t>(trim_sv(s)).value_or(0ULL); }, 0ULL) };
            if (max_f > 0) { return FreqInfo { max_f, true }; }

            if (auto online = read_file(e.path() / "online"); online && trim_sv(*online) == "0") {
                return std::nullopt;
            }

            const auto scale_f { parse_file_or<std::uint64_t>(
                e.path() / "cpufreq/scaling_max_freq",
                [](auto s) { return parse_number<std::uint64_t>(trim_sv(s)).value_or(0ULL); }, 0ULL) };
            if (scale_f > 0) { return FreqInfo { scale_f, false }; }

            return std::nullopt;
        };

        auto cores = fs::directory_iterator("/sys/devices/system/cpu", ec) | std::views::transform(read_core)
            | std::views::filter([](auto o) { return o.has_value(); })
            | std::views::transform([](auto o) { return *o; });

        auto result = std::ranges::fold_left(cores, FreqInfo {}, [](FreqInfo a, FreqInfo b) {
            return std::ranges::max(a, b, {}, [](auto f) { return std::make_pair(f.khz, f.is_true_max); });
        });
        if (result.khz > 0) return result;
    }

    auto items = kCpuInfoProbe | split_to_sv('\n') | std::views::filter([](auto l) { return l.starts_with("cpu MHz"); })
        | std::views::transform([](auto l) -> std::uint64_t {
              const auto c { l.find(':') };
              if (c == std::string_view::npos) return 0ULL;
              const auto s { trim_sv(l.substr(c + 1)) };
              return parse_number<long double>(s).transform([](auto f) { return toULong(f * 1000.0L); }).value_or(0ULL);
          });

    auto max_f = std::ranges::fold_left(
        items, 0ULL, [](std::uint64_t a, std::uint64_t b) { return std::max<std::uint64_t>(a, b); });
    return { max_f, false };
}();

inline const std::string kVirtualizationProbe = []() -> std::string {
    using namespace std::string_view_literals;

    // --- Tier 1: Process Scope (The Tip) ---
    // Detect if we are running under an emulator process or have environment markers.
    static constexpr auto kQemuEnvs = std::to_array<std::string_view>(
        { "QEMU_LD_PREFIX"sv, "QEMU_SET_ENV"sv, "QEMU_RESERVED_VA"sv, "CROSS_RUNNER"sv });
    if (std::ranges::any_of(kQemuEnvs, [](auto e) { return std::getenv(e.data()); })) { return "QEMU (Emulated)"; }

    if (auto m = read_file("/proc/self/maps"); m && (m->contains("/usr/bin/qemu") || m->contains("/bin/qemu-"))) {
        return "QEMU (Emulated)";
    }

    std::array<char, 4096> exe_path {};
    if (auto len = posix::readlink("/proc/self/exe", exe_path); len && *len < exe_path.size()) {
        if (const std::string_view exe_sv(exe_path.data(), *len);
            exe_sv.contains("/usr/bin/qemu") || exe_sv.contains("/bin/qemu-")) {
            return "QEMU (Emulated)";
        }
    }

    // --- Tier 2: Isolation Scope (Container/Namespace) ---
    // Detect shared-kernel virtualization (containers).
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists("/.dockerenv", ec)) { return "Docker"; }
    if (fs::exists("/run/.containerenv", ec)) { return "Podman"; }
    if (fs::exists("/proc/user_beancounters", ec)) { return "OpenVZ"; }

    if (auto env1 = read_file("/proc/1/environ")) {
        auto parts        = *env1 | split_to_sv('\0');
        const auto is_lxc = [](std::string_view part) noexcept { return part == "container=lxc"sv; };
        const auto is_wsl = [](std::string_view part) noexcept {
            return part.starts_with("WSL_DISTRO_NAME=") || part.starts_with("WSL_INTEROP=")
                || part.starts_with("WSLENV=");
        };

        auto check_tag = [&parts](auto pred, std::string_view tag) -> std::optional<std::string_view> {
            return std::ranges::any_of(parts, pred) ? std::optional { tag } : std::nullopt;
        };

        if (auto res
            = check_tag(is_lxc, "LXC"sv).or_else([&check_tag, &is_wsl] { return check_tag(is_wsl, "WSL"sv); })) {
            return std::string(*res);
        }
    }

    // --- Tier 3: Kernel Scope (OS/Subsystem) ---
    // Detect specific OS implementations or kernel-reported hypervisors.
    if (kUnameProbe) {
        auto canon = [](std::string_view s) {
            if (s.contains("x86_64") || s.contains("amd64")) { return "x64"sv; }
            if (s.contains("aarch64") || s.contains("arm64")) { return "arm64"sv; }
            return s;
        };
        const std::string_view machine { kUnameProbe->machine };
        const std::string_view release { kUnameProbe->release };
        const std::string_view version { kUnameProbe->version };
        const std::string_view garch { canon(machine) };
        static constexpr auto kArches = std::to_array<std::string_view>(
            { "x86_64"sv, "amd64"sv, "aarch64"sv, "arm64"sv, "riscv64"sv, "ppc64"sv, "s390x"sv });
        if (std::ranges::any_of(kArches, [garch, release, version, canon](auto a) {
                return (release.contains(a) || version.contains(a)) && garch != canon(a);
            })) {
            return "QEMU (Emulated)";
        }

        if (release.contains("Microsoft") || release.contains("WSL")) { return "WSL"; }
    }

    if (fs::exists("/dev/dxg", ec) || fs::exists("/dev/lxss", ec) || fs::exists("/usr/lib/wsl", ec)) { return "WSL"; }

    if (auto hib = parse_file_or<std::string>(
            "/sys/hypervisor/type", [](auto s) { return std::string { trim_sv(s) }; }, std::string {});
        !hib.empty()) {
        if (hib == "xen") { return "Xen"; }
        if (hib == "kvm") { return "KVM"; }
    }

    // --- Tier 4: Hardware Scope (Silicon Level) ---
    // Direct hardware queries (CPUID) and CPU feature flags.
    bool hv_bit { false };
#if defined(__x86_64__) || defined(__i386__)
    std::uint32_t ex {}, bx {}, cx {}, dx {};
    __cpuid(1, ex, bx, cx, dx);
    hv_bit = (cx & (std::uint32_t { 1 } << 31)) != 0;
    if (hv_bit) {
        __cpuid(0x40000000, ex, bx, cx, dx);
        auto cast = [](std::uint32_t v) { return std::bit_cast<std::array<char, 4>>(v); };
        std::array<char, 13> s {};
        std::ranges::copy(cast(bx), s.begin());
        std::ranges::copy(cast(cx), s.begin() + 4);
        std::ranges::copy(cast(dx), s.begin() + 8);
        const std::string_view sig { s.data() };

        struct HvRule {
            std::string_view signature;
            std::string_view result;
        };
        static constexpr auto kHvRules = std::to_array<HvRule>({ { "KVMKVMKVM", "KVM" }, { "Microsoft Hv", "Hyper-V" },
            { "VMwareVMware", "VMware" }, { "XenVMMXenVMM", "Xen" }, { "VBoxVBoxVBox", "VirtualBox" },
            { "TCGTCGTCGTCG", "QEMU" }, { "bhyve bhyve ", "Bhyve" }, { " QNXQVMBSQG ", "QNX" },
            { " prl hyperv ", "Parallels" }, { " lrpepyh vr ", "Parallels" }, { "prl hyperv  ", "Parallels" } });

        if (auto it = std::ranges::find_if(kHvRules, [&sig](const auto& r) { return r.signature == sig; });
            it != kHvRules.end())
            return std::string(it->result);
    }
#endif

    if (kCpuInfoProbe.contains("QEMU") || kCpuInfoProbe.contains("implementer\t: 0x00")) return "QEMU (Emulated)";

    // --- Tier 5: Firmware/Global Scope (The Base) ---
    // Detect identity from DMI/SMBIOS or Device Tree.
    {
        auto check_dmi = [](std::string_view path) {
            return parse_file_or<std::string>(
                path.data(), [](auto s) { return std::string { trim_sv(s) }; }, std::string {});
        };
        const std::string v { check_dmi("/sys/class/dmi/id/sys_vendor") };
        const std::string p { check_dmi("/sys/class/dmi/id/product_name") };

        struct DmiRule {
            std::string_view result;
            bool (*match)(std::string_view, std::string_view);
        };
        static constexpr auto kDmiRules = std::to_array<DmiRule>({ { "Hyper-V",
                                                                       [](auto v, auto p) {
                                                                           return v.contains("Microsoft")
                                                                               && (p.contains("Virtual Machine")
                                                                                   || p.contains("VirtualPC"));
                                                                       } },
            { "VMware", [](auto v, auto p) { return v.contains("VMware") || p.contains("VMware"); } },
            { "VirtualBox",
                [](auto v, auto p) {
                    return v.contains("Oracle") || v.contains("innotek") || p.contains("VirtualBox");
                } },
            { "KVM",
                [](auto v, auto p) {
                    return v.contains("KVM") || p.contains("KVM")
                        || (v.contains("Red Hat") && (p.contains("KVM") || p.contains("RHEL")));
                } },
            { "QEMU", [](auto v, auto p) { return v.contains("QEMU") || p.contains("QEMU") || p.contains("Bochs"); } },
            { "Xen", [](auto v, auto p) { return v.contains("Xen") || p.contains("HVM domU"); } },
            { "Parallels", [](auto v, auto p) { return v.contains("Parallels") || p.contains("Parallels"); } },
            { "Amazon EC2", [](auto v, auto) { return v.contains("Amazon EC2"); } },
            { "Google Compute Engine", [](auto, auto p) { return p.contains("Google Compute Engine"); } } });

        if (auto it = std::ranges::find_if(kDmiRules, [&v, &p](const auto& r) { return r.match(v, p); });
            it != kDmiRules.end()) {
            return std::string(it->result);
        }
    }

    if (auto dt_model = parse_file_or<std::string>(
            "/proc/device-tree/model", [](auto s) { return std::string { trim_sv(s) }; }, std::string {});
        !dt_model.empty()) {
        if (dt_model.contains("QEMU")) { return "QEMU"; }
        if (dt_model.contains("KVM")) { return "KVM"; }
    }

    return hv_bit ? "Dedicated (Virtual)" : "Dedicated";
}();

} // namespace probe

/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "system_probe.hpp"

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
#include <type_traits>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

namespace probe {

namespace {

template <string_utils::FixedString... Envs> [[nodiscard]] bool check_envs() noexcept {
    return ((std::getenv(Envs.chars.data()) != nullptr) || ...);
}

template <string_utils::FixedString... Prefixes>
[[nodiscard]] constexpr bool starts_with_any(std::string_view str) noexcept {
    return (str.starts_with(std::string_view { Prefixes.chars.data(), Prefixes.size() }) || ...);
}

[[nodiscard]] std::expected<::utsname, std::error_code> probe_uname() noexcept {
    return posix::uname();
}

[[nodiscard]] constexpr std::uint8_t detect_machine_bits(std::string_view raw) noexcept {
    if (raw.contains("64") || raw == "s390x") { return 64; }
    if (raw.contains("86") || raw.starts_with("arm")) { return 32; }
    return toUByte(std::numeric_limits<std::uintptr_t>::digits);
}

[[nodiscard]] ArchInfo format_arch_info(const ::utsname& res) {
    const std::string raw { res.machine };
    const auto bits = detect_machine_bits(raw);
    return ArchInfo { raw, std::format("{} ({} Bit)", raw, bits) };
}

[[nodiscard]] ArchInfo probe_arch() {
    return get_uname_probe().transform(format_arch_info).value_or(ArchInfo { "unknown", "Unknown" });
}

[[nodiscard]] std::string probe_cpu_info() {
    return read_file("/proc/cpuinfo").value_or(std::string {});
}

[[nodiscard]] std::string probe_os_release() {
    return read_file("/etc/os-release").value_or(std::string {});
}

[[nodiscard]] std::string parse_tcp_cc(std::string_view raw_cc_sv) {
    return std::string { trim_sv(raw_cc_sv) };
}

[[nodiscard]] std::string probe_tcp_cc() {
    return parse_file_or<std::string>("/proc/sys/net/ipv4/tcp_congestion_control", parse_tcp_cc, std::string {});
}

struct CacheEntry {
    std::uint32_t level {};
    bool is_data {};
    std::string size {};
};

struct UnitMultiplier {
    char unit {};
    std::uint64_t multiplier {};
};

static constexpr auto kUnits = std::to_array<UnitMultiplier>(
    { { 'G', 1024ULL * 1024 * 1024 }, { 'g', 1024ULL * 1024 * 1024 }, { 'M', 1024ULL * 1024 }, { 'm', 1024ULL * 1024 },
        { 'K', 1024ULL }, { 'k', 1024ULL }, { 'B', 1ULL }, { 'b', 1ULL } });

[[nodiscard]] constexpr std::uint64_t get_multiplier(char unit_char) noexcept {
    const auto unit_it = std::ranges::find_if(
        kUnits, [unit_char](const auto& unit_multiplier) { return unit_multiplier.unit == unit_char; });
    if (unit_it != kUnits.end()) { return unit_it->multiplier; }
    return 1ULL;
}

[[nodiscard]] std::string parse_cache_size(std::string_view size_sv) {
    size_sv = trim_sv(size_sv);
    if (size_sv.empty()) { return "Unknown"; }

    std::uint64_t parsed_bytes {};
    const auto [end_pointer, error_status]
        = std::from_chars(size_sv.data(), size_sv.data() + size_sv.size(), parsed_bytes);
    if (error_status != std::errc {}) { return std::string { size_sv }; }

    const std::string_view suffix_part(end_pointer, size_sv.data() + size_sv.size());
    const auto suffix_trimmed = trim_sv(suffix_part);
    const auto multiplier     = suffix_trimmed.empty() ? 1024ULL : get_multiplier(suffix_trimmed.front());

    return safe_mul(multiplier, parsed_bytes).transform(format_bytes).value_or(std::string { size_sv });
}

[[nodiscard]] constexpr bool is_unified_or_data(std::string_view type_str) noexcept {
    return type_str == "Unified" || type_str == "Data";
}

[[nodiscard]] std::optional<CacheEntry> choose_better_cache(
    std::optional<CacheEntry> best, std::uint32_t level_val, bool data, std::string size_str) {
    if (!best || level_val > best->level || (level_val == best->level && data && !best->is_data)) {
        return CacheEntry { level_val, data, std::move(size_str) };
    }
    return best;
}

[[nodiscard]] std::optional<CacheEntry> parse_cache_entry(const std::filesystem::directory_entry& entry) {
    const auto size_str = read_file(entry.path() / "size");
    if (!size_str) { return std::nullopt; }

    const auto level_val = parse_file_or<std::uint32_t>(
        entry.path() / "level",
        [](const auto level_content_sv) { return parse_number<std::uint32_t>(trim_sv(level_content_sv)).value_or(0U); },
        0U);
    const auto type_str = parse_file_or<std::string>(
        entry.path() / "type", [](const auto type_content_sv) { return std::string { trim_sv(type_content_sv) }; },
        std::string {});

    return CacheEntry { level_val, is_unified_or_data(type_str), std::move(*size_str) };
}

[[nodiscard]] std::optional<CacheEntry> scan_cpu_cache_dir() {
    namespace fs = std::filesystem;
    static constexpr std::string_view kCacheDir { "/sys/devices/system/cpu/cpu0/cache" };
    std::error_code ec {};
    if (!fs::exists(kCacheDir, ec)) { return std::nullopt; }

    auto directory_range = fs::directory_iterator(kCacheDir, ec);
    if (ec) { return std::nullopt; }

    auto valid_entries = directory_range | std::views::filter([](const auto& entry) {
        std::error_code entry_ec {};
        return entry.is_directory(entry_ec);
    }) | std::views::filter([](const auto& entry) { return entry.path().filename().string().starts_with("index"); })
        | std::views::transform(parse_cache_entry)
        | std::views::filter([](const auto& entry_opt) { return entry_opt.has_value(); })
        | std::views::transform([](const auto& entry_opt) { return *entry_opt; });

    return std::ranges::fold_left(
        valid_entries, std::optional<CacheEntry> {}, [](std::optional<CacheEntry> best, CacheEntry current) {
            return choose_better_cache(std::move(best), current.level, current.is_data, std::move(current.size));
        });
}

[[nodiscard]] std::string probe_cpu_cache() {
    const auto best = scan_cpu_cache_dir();
    if (!best) { return "Unknown"; }
    return parse_cache_size(best->size);
}

[[nodiscard]] std::string parse_dt_model(std::string_view model_raw_sv) {
    const auto trimmed_sv { trim_sv(model_raw_sv) };
    return std::string { trimmed_sv.substr(0, trimmed_sv.find('\0')) };
}

[[nodiscard]] std::string probe_dt_model() {
    return parse_file_or<std::string>("/sys/firmware/devicetree/base/model", parse_dt_model, std::string {});
}

[[nodiscard]] std::string parse_midr(std::string_view midr_raw_sv) {
    return std::string { trim_sv(midr_raw_sv) };
}

[[nodiscard]] std::string probe_midr() {
    return parse_file_or<std::string>(
        "/sys/devices/system/cpu/cpu0/regs/identification/midr_el1", parse_midr, std::string {});
}

[[nodiscard]] bool parse_zswap_enabled(std::string_view zswap_raw_sv) noexcept {
    const auto trimmed_sv { trim_sv(zswap_raw_sv) };
    return trimmed_sv == "Y" || trimmed_sv == "y" || trimmed_sv == "1";
}

[[nodiscard]] bool probe_zswap_enabled() {
    return parse_file_or<bool>("/sys/module/zswap/parameters/enabled", parse_zswap_enabled, false);
}

[[nodiscard]] std::optional<FreqInfo> probe_max_freq_cpuid() noexcept {
#if defined(__i386__) || defined(__x86_64__)
    if (__get_cpuid_max(0, nullptr) >= 0x16) {
        std::uint32_t eax_val {};
        std::uint32_t ebx_val {};
        std::uint32_t ecx_val {};
        std::uint32_t edx_val {};
        __cpuid(0x16, eax_val, ebx_val, ecx_val, edx_val);
        if (std::uint64_t freq_mhz { ebx_val & 0xFFFFU }; freq_mhz > 0U) { return FreqInfo { freq_mhz * 1000U, true }; }
    }
#endif
    return std::nullopt;
}

[[nodiscard]] std::optional<FreqInfo> read_core_freq(const std::filesystem::directory_entry& cpu_entry) {
    namespace fs = std::filesystem;
    std::error_code dir_ec {};
    if (!cpu_entry.is_directory(dir_ec) || dir_ec) { return std::nullopt; }
    const auto fname = cpu_entry.path().filename().string();
    if (!fname.starts_with("cpu")) { return std::nullopt; }
    const std::string_view suffix { std::string_view { fname }.substr(3) };
    if (suffix.empty()
        || !std::ranges::all_of(suffix, [](unsigned char digit_char) { return std::isdigit(digit_char) != 0; })) {
        return std::nullopt;
    }

    if (const auto online = read_file(cpu_entry.path() / "online"); online && trim_sv(*online) == "0") {
        return std::nullopt;
    }

    const auto max_freq { parse_file_or<std::uint64_t>(
        cpu_entry.path() / "cpufreq/cpuinfo_max_freq",
        [](const auto content_sv) { return parse_number<std::uint64_t>(trim_sv(content_sv)).value_or(0ULL); }, 0ULL) };
    if (max_freq > 0) { return FreqInfo { max_freq, true }; }

    const auto scale_freq { parse_file_or<std::uint64_t>(
        cpu_entry.path() / "cpufreq/scaling_max_freq",
        [](const auto content_sv) { return parse_number<std::uint64_t>(trim_sv(content_sv)).value_or(0ULL); }, 0ULL) };
    if (scale_freq > 0) { return FreqInfo { scale_freq, false }; }

    return std::nullopt;
}

[[nodiscard]] std::optional<FreqInfo> probe_max_freq_sysfs() {
    namespace fs = std::filesystem;
    std::error_code ec {};
    if (!fs::exists("/sys/devices/system/cpu", ec)) { return std::nullopt; }
    auto cores = fs::directory_iterator("/sys/devices/system/cpu", ec) | std::views::transform(read_core_freq)
        | std::views::filter([](const auto& freq_opt) { return freq_opt.has_value(); })
        | std::views::transform([](const auto& freq_opt) { return *freq_opt; });

    const auto result = std::ranges::fold_left(cores, FreqInfo {}, [](FreqInfo acc_freq, FreqInfo curr_freq) {
        return std::ranges::max(
            acc_freq, curr_freq, {}, [](const auto& freq) { return std::make_pair(freq.khz, freq.is_true_max); });
    });
    if (result.khz > 0) { return result; }
    return std::nullopt;
}

[[nodiscard]] std::uint64_t parse_cpu_mhz_line(std::string_view line_sv) noexcept {
    const auto colon_pos { line_sv.find(':') };
    if (colon_pos == std::string_view::npos) { return 0ULL; }
    const auto freq_val_sv { trim_sv(line_sv.substr(colon_pos + 1)) };
    return parse_number<long double>(freq_val_sv)
        .transform([](const auto freq_val) { return toULong(freq_val * 1000.0L); })
        .value_or(0ULL);
}

[[nodiscard]] FreqInfo probe_max_freq_cpuinfo() {
    auto items = get_cpu_info_probe() | split_to_sv('\n')
        | std::views::filter([](const auto line_sv) { return line_sv.starts_with("cpu MHz"); })
        | std::views::transform(parse_cpu_mhz_line);

    const auto max_f = std::ranges::fold_left(items, 0ULL,
        [](std::uint64_t max_val, std::uint64_t curr_val) { return std::max<std::uint64_t>(max_val, curr_val); });
    return { max_f, false };
}

[[nodiscard]] FreqInfo probe_max_freq() {
    if (const auto cpuid_res = probe_max_freq_cpuid()) { return *cpuid_res; }
    if (const auto sysfs_res = probe_max_freq_sysfs()) { return *sysfs_res; }
    return probe_max_freq_cpuinfo();
}

[[nodiscard]] bool check_qemu_envs() noexcept {
    return check_envs<"QEMU_LD_PREFIX", "QEMU_SET_ENV", "QEMU_RESERVED_VA", "CROSS_RUNNER">();
}

[[nodiscard]] bool check_qemu_maps() {
    const auto maps_content = read_file("/proc/self/maps");
    return maps_content && (maps_content->contains("/usr/bin/qemu") || maps_content->contains("/bin/qemu-"));
}

[[nodiscard]] bool check_qemu_exe() noexcept {
    std::array<char, 4096> exe_path {};
    const auto len = posix::readlink("/proc/self/exe", exe_path);
    if (!len || *len >= exe_path.size()) { return false; }
    const std::string_view exe_sv(exe_path.data(), *len);
    return exe_sv.contains("/usr/bin/qemu") || exe_sv.contains("/bin/qemu-");
}

/**
 * @brief Tier 1 (Process Scope): Fast user-space virtualization checks.
 * @details Inspects process-level properties (environment variables, mapped memory,
 *          and /proc/self/exe path) to detect cheap-to-probe QEMU emulation.
 */
[[nodiscard]] std::optional<std::string> detect_process_scope() {
    if (check_qemu_envs() || check_qemu_maps() || check_qemu_exe()) { return "QEMU (Emulated)"; }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> check_proc_environ() {
    const auto init_environ = read_file("/proc/1/environ");
    if (!init_environ) { return std::nullopt; }
    auto parts = *init_environ | split_to_sv('\0');
    if (std::ranges::any_of(parts, [](const auto part) { return part == "container=lxc"; })) { return "LXC"; }
    if (std::ranges::any_of(parts,
            [](const auto part) { return starts_with_any<"WSL_DISTRO_NAME=", "WSL_INTEROP=", "WSLENV=">(part); })) {
        return "WSL";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> detect_container_type() noexcept {
    namespace fs = std::filesystem;
    std::error_code ec {};
    if (fs::exists("/.dockerenv", ec)) { return "Docker"; }
    if (fs::exists("/run/.containerenv", ec)) { return "Podman"; }
    if (fs::exists("/proc/user_beancounters", ec)) { return "OpenVZ"; }
    return std::nullopt;
}

/**
 * @brief Tier 2 (Isolation Scope): Containerization checks.
 * @details Inspects container indicators (e.g., /.dockerenv, /run/.containerenv)
 *          and environment variables of the init process (PID 1) to detect Docker,
 *          Podman, OpenVZ, LXC, or WSL.
 */
[[nodiscard]] std::optional<std::string> detect_isolation_scope() {
    return detect_container_type().or_else(check_proc_environ);
}

[[nodiscard]] constexpr std::string_view canon_arch(std::string_view arch_sv) noexcept {
    if (arch_sv.contains("x86_64") || arch_sv.contains("amd64")) { return "x64"; }
    if (arch_sv.contains("aarch64") || arch_sv.contains("arm64")) { return "arm64"; }
    return arch_sv;
}

[[nodiscard]] constexpr bool is_qemu_kernel_emulated(
    std::string_view machine, std::string_view release, std::string_view version) noexcept {
    const std::string_view garch { canon_arch(machine) };
    static constexpr auto kArches
        = std::to_array<std::string_view>({ "x86_64", "amd64", "aarch64", "arm64", "riscv64", "ppc64", "s390x" });
    return std::ranges::any_of(kArches, [garch, release, version](const auto target_arch) {
        return (release.contains(target_arch) || version.contains(target_arch)) && garch != canon_arch(target_arch);
    });
}

[[nodiscard]] std::optional<std::string> check_uname_kernel_impl(const ::utsname& uts) noexcept {
    const std::string_view machine { uts.machine };
    const std::string_view release { uts.release };
    const std::string_view version { uts.version };
    if (is_qemu_kernel_emulated(machine, release, version)) { return "QEMU (Emulated)"; }
    if (release.contains("Microsoft") || release.contains("WSL")) { return "WSL"; }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> check_uname_kernel() noexcept {
    return get_uname_probe().transform(check_uname_kernel_impl).value_or(std::nullopt);
}

[[nodiscard]] std::optional<std::string> check_wsl_devices() noexcept {
    namespace fs = std::filesystem;
    std::error_code ec {};
    if (fs::exists("/dev/dxg", ec) || fs::exists("/dev/lxss", ec) || fs::exists("/usr/lib/wsl", ec)) { return "WSL"; }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> check_hypervisor_type() {
    const auto hypervisor_type = parse_file_or<std::string>(
        "/sys/hypervisor/type", [](const auto content_sv) { return std::string { trim_sv(content_sv) }; },
        std::string {});
    if (hypervisor_type == "xen") { return "Xen"; }
    if (hypervisor_type == "kvm") { return "KVM"; }
    return std::nullopt;
}

/**
 * @brief Tier 3 (Kernel Scope): Kernel and operating system checks.
 * @details Checks kernel version signatures (uname release fields), WSL devices,
 *          and sysfs hypervisor type entries (Xen, KVM).
 */
[[nodiscard]] std::optional<std::string> detect_kernel_scope() {
    return check_uname_kernel().or_else(check_wsl_devices).or_else(check_hypervisor_type);
}

[[nodiscard]] std::optional<std::string> check_cpuid_hypervisor() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    if (get_cpu_features().has_hv_bit) {
        std::uint32_t eax_val {}, ebx_val {}, ecx_val {}, edx_val {};
        __cpuid(0x40000000, eax_val, ebx_val, ecx_val, edx_val);
        const auto cast = [](std::uint32_t v) { return std::bit_cast<std::array<char, 4>>(v); };
        std::array<char, 13> sig_chars {};
        std::ranges::copy(cast(ebx_val), sig_chars.begin());
        std::ranges::copy(cast(ecx_val), sig_chars.begin() + 4);
        std::ranges::copy(cast(edx_val), sig_chars.begin() + 8);
        const std::string_view sig { sig_chars.data() };

        struct HvRule {
            std::string_view signature {};
            std::string_view result {};
        };
        static constexpr auto kHvRules = std::to_array<HvRule>({ { "KVMKVMKVM", "KVM" }, { "Microsoft Hv", "Hyper-V" },
            { "VMwareVMware", "VMware" }, { "XenVMMXenVMM", "Xen" }, { "VBoxVBoxVBox", "VirtualBox" },
            { "TCGTCGTCGTCG", "QEMU" }, { "bhyve bhyve ", "Bhyve" }, { " QNXQVMBSQG ", "QNX" },
            { " prl hyperv ", "Parallels" }, { " lrpepyh vr ", "Parallels" }, { "prl hyperv  ", "Parallels" } });

        if (const auto it = std::ranges::find_if(kHvRules, [&sig](const auto& rule) { return rule.signature == sig; });
            it != kHvRules.end()) {
            return std::string(it->result);
        }
    }
#endif
    return std::nullopt;
}

/**
 * @brief Tier 4 (Hardware Scope): CPUID and hardware architecture checks.
 * @details Checks the hypervisor present bit in CPUID registers and maps signatures,
 *          with fallback checks for arm64 cpuinfo implementer.
 */
[[nodiscard]] std::optional<std::string> detect_hardware_scope() {
    if (const auto res = check_cpuid_hypervisor()) { return res; }
    if (get_cpu_info_probe().contains("QEMU") || get_cpu_info_probe().contains("implementer\t: 0x00")) {
        return "QEMU (Emulated)";
    }
    return std::nullopt;
}

[[nodiscard]] std::string read_dmi_info(std::string_view path) {
    return parse_file_or<std::string>(
        path.data(), [](const auto content_sv) { return std::string { trim_sv(content_sv) }; }, std::string {});
}

struct DmiRule {
    std::string_view result {};
    std::add_pointer_t<bool(std::string_view, std::string_view)> match {};
};

static constexpr auto kDmiRules = std::to_array<DmiRule>({ { "Hyper-V",
                                                               [](const auto vendor, const auto product) {
                                                                   return vendor.contains("Microsoft")
                                                                       && (product.contains("Virtual Machine")
                                                                           || product.contains("VirtualPC"));
                                                               } },
    { "VMware",
        [](const auto vendor, const auto product) { return vendor.contains("VMware") || product.contains("VMware"); } },
    { "VirtualBox",
        [](const auto vendor, const auto product) {
            return vendor.contains("Oracle") || vendor.contains("innotek") || product.contains("VirtualBox");
        } },
    { "KVM",
        [](const auto vendor, const auto product) {
            return vendor.contains("KVM") || product.contains("KVM")
                || (vendor.contains("Red Hat") && (product.contains("KVM") || product.contains("RHEL")));
        } },
    { "QEMU",
        [](const auto vendor, const auto product) {
            return vendor.contains("QEMU") || product.contains("QEMU") || product.contains("Bochs");
        } },
    { "Xen",
        [](const auto vendor, const auto product) { return vendor.contains("Xen") || product.contains("HVM domU"); } },
    { "Parallels",
        [](const auto vendor, const auto product) {
            return vendor.contains("Parallels") || product.contains("Parallels");
        } },
    { "Amazon EC2", [](const auto vendor, const auto) { return vendor.contains("Amazon EC2"); } },
    { "Google Compute Engine",
        [](const auto, const auto product) { return product.contains("Google Compute Engine"); } } });

[[nodiscard]] std::optional<std::string> check_dmi_rules() {
    const std::string vendor { read_dmi_info("/sys/class/dmi/id/sys_vendor") };
    const std::string product { read_dmi_info("/sys/class/dmi/id/product_name") };

    if (const auto it = std::ranges::find_if(
            kDmiRules, [&vendor, &product](const auto& rule) { return rule.match(vendor, product); });
        it != kDmiRules.end()) {
        return std::string(it->result);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> check_device_tree_model() {
    const auto dt_model = parse_file_or<std::string>(
        "/proc/device-tree/model", [](const auto content_sv) { return std::string { trim_sv(content_sv) }; },
        std::string {});
    if (dt_model.contains("QEMU")) { return "QEMU"; }
    if (dt_model.contains("KVM")) { return "KVM"; }
    return std::nullopt;
}

/**
 * @brief Tier 5 (Global Scope): Firmware and motherboard/device tree check.
 * @details Parses system DMI product data, /sys firmware tables, and device-tree models;
 *          falls back to a generic dedicated-hardware label using the pre-computed CPUID
 *          hypervisor-present bit (see @p hv_bit).
 */
[[nodiscard]] std::optional<std::string> detect_global_scope(bool hv_bit) {
    return check_dmi_rules().or_else(check_device_tree_model).or_else([hv_bit]() -> std::optional<std::string> {
        return hv_bit ? "Dedicated (Virtual)" : "Dedicated";
    });
}

/**
 * @brief Probes the virtualization/isolation environment of the system.
 * @details Coordinates the tiered virtualization fallback structure (Tier 1 to Tier 5),
 *          implementing a lazy cheap-to-expensive evaluation chain to avoid CPU and I/O overhead.
 */
[[nodiscard]] std::string probe_virtualization() {
    return detect_process_scope()
        .or_else(detect_isolation_scope)
        .or_else(detect_kernel_scope)
        .or_else(detect_hardware_scope)
        .or_else([]() { return detect_global_scope(get_cpu_features().has_hv_bit); })
        .value_or("");
}

} // namespace

const std::expected<::utsname, std::error_code>& get_uname_probe() noexcept {
    static const auto instance = probe_uname();
    return instance;
}

const ArchInfo& get_arch_probe() noexcept {
    static const auto instance = probe_arch();
    return instance;
}

const std::string& get_cpu_info_probe() noexcept {
    static const auto instance = probe_cpu_info();
    return instance;
}

const std::string& get_os_release_probe() noexcept {
    static const auto instance = probe_os_release();
    return instance;
}

const std::string& get_tcp_cc_probe() noexcept {
    static const auto instance = probe_tcp_cc();
    return instance;
}

const std::string& get_cpu_cache_probe() noexcept {
    static const auto instance = probe_cpu_cache();
    return instance;
}

const std::string& get_dt_model_probe() noexcept {
    static const auto instance = probe_dt_model();
    return instance;
}

const std::string& get_midr_probe() noexcept {
    static const auto instance = probe_midr();
    return instance;
}

bool get_zswap_enabled_probe() noexcept {
    static const bool instance = probe_zswap_enabled();
    return instance;
}

const FreqInfo& get_max_freq_probe() noexcept {
    static const auto instance = probe_max_freq();
    return instance;
}

const std::string& get_virtualization_probe() noexcept {
    static const auto instance = probe_virtualization();
    return instance;
}

const CpuFeatures& get_cpu_features() noexcept {
    static const auto instance = []() noexcept {
#if defined(__i386__) || defined(__x86_64__)
        std::uint32_t cpuid1_eax {}, cpuid1_ebx {}, cpuid1_ecx {}, cpuid1_edx {};
        const bool cpuid1_ok = __get_cpuid(1, &cpuid1_eax, &cpuid1_ebx, &cpuid1_ecx, &cpuid1_edx) != 0;

        const bool has_hv_bit    = cpuid1_ok && (cpuid1_ecx & (1U << 31)) != 0;
        const bool has_aes       = cpuid1_ok && (cpuid1_ecx & (1U << 25)) != 0;
        const bool has_vmx_intel = cpuid1_ok && (cpuid1_ecx & (1U << 5)) != 0;

        const auto check_amd_svm = []() noexcept -> bool {
            const std::uint32_t max_ext = __get_cpuid_max(0x80000000, nullptr);
            if (max_ext < 0x80000001) { return false; }
            std::uint32_t eax {}, ebx {}, ecx {}, edx {};
            __cpuid(0x80000001, eax, ebx, ecx, edx);
            return (ecx & (1U << 2)) != 0;
        };

        const bool has_vmx = has_vmx_intel || check_amd_svm();

        return CpuFeatures { .has_aes = has_aes, .has_vmx = has_vmx, .has_hv_bit = has_hv_bit };
#else
        return CpuFeatures { .has_aes = cpu_has_flag("aes"),
            .has_vmx                  = cpu_has_flag("vmx") || cpu_has_flag("svm") || cpu_has_flag("virt"),
            .has_hv_bit               = false };
#endif
    }();
    return instance;
}

bool cpu_has_flag(std::string_view flag) noexcept {
    const auto& cpuinfo = get_cpu_info_probe();
    if (cpuinfo.empty()) { return false; }

    constexpr std::array<std::string_view, 2> kKeys = { "flags", "Features" };
    const auto field_opt                            = lookup_info_field(cpuinfo, kKeys);
    if (!field_opt) { return false; }

    auto flag_words = *field_opt | tokenize_sv();
    return std::ranges::any_of(flag_words, [flag](auto word) { return !word.empty() && word == flag; });
}

} // namespace probe

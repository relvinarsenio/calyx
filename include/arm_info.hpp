/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "system_probe.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace arm {

/**
 * @brief ARM CPU identification logic via MIDR_EL1 fields.
 *
 * @details
 * Sources:
 * 1. /proc/cpuinfo "CPU implementer" + "CPU part" hex fields.
 * 2. /sys/devices/system/cpu/cpu0/regs/identification/midr_el1 (available since kernel 4.11).
 *
 * MIDR_EL1 register layout (ARM Architecture Reference Manual):
 * - [31:24] Implementer
 * - [23:20] Variant
 * - [19:16] Architecture
 * - [15:4]  PartNum
 * - [3:0]   Revision
 *
 * Lookup data sourced from: Linux kernel source code (arch/arm64/kernel/cpuinfo.c),
 * util-linux lscpu-arm.c, pytorch/cpuinfo, Apple XNU (osfmk/arm/cpuid.h),
 * Asahi Linux (M-series RE), LLVM AArch64 backend, and Microsoft Azure Cobalt technical documentation.
 */

/**
 * @struct ArmPartEntry
 * @brief Represents a specific ARM CPU part ID and its human-readable name.
 */
struct ArmPartEntry {
    std::int32_t part_id {};
    std::string_view name {};
};

/**
 * @struct ArmImplEntry
 * @brief Represents an ARM CPU implementer (vendor) and their associated part numbers.
 */
struct ArmImplEntry {
    std::int32_t impl_id {};
    std::string_view vendor {};
    std::span<const ArmPartEntry> parts {};
};

static constexpr ArmPartEntry kArmParts[] = {
    // --- Classic 32-bit legacy cores (ARMv4 through ARMv6 architectures) ---
    { 0x810, "ARM810" },
    { 0x920, "ARM920" },
    { 0x922, "ARM922" },
    { 0x926, "ARM926" },
    { 0x940, "ARM940" },
    { 0x946, "ARM946" },
    { 0x966, "ARM966" },
    { 0xa20, "ARM1020" },
    { 0xa22, "ARM1022" },
    { 0xa26, "ARM1026" },
    { 0xb02, "ARM11-MPCore" },
    { 0xb36, "ARM1136" },
    { 0xb56, "ARM1156" },
    { 0xb76, "ARM1176" },
    // --- 32-bit Cortex-A/R/M series (ARMv7-A/R/M implementations) ---
    { 0xc05, "Cortex-A5" },
    { 0xc07, "Cortex-A7" },
    { 0xc08, "Cortex-A8" },
    { 0xc09, "Cortex-A9" },
    { 0xc0d, "Cortex-A17" },
    { 0xc0f, "Cortex-A15" },
    { 0xc0e, "Cortex-A17" },
    { 0xc14, "Cortex-R4" },
    { 0xc15, "Cortex-R5" },
    { 0xc17, "Cortex-R7" },
    { 0xc18, "Cortex-R8" },
    { 0xc20, "Cortex-M0" },
    { 0xc21, "Cortex-M1" },
    { 0xc23, "Cortex-M3" },
    { 0xc24, "Cortex-M4" },
    { 0xc27, "Cortex-M7" },
    { 0xc60, "Cortex-M0+" },
    // --- 64-bit foundation: ARMv8-A and early ARMv9-A base designs ---
    { 0xd01, "Cortex-A32" },
    { 0xd02, "Cortex-A34" },
    { 0xd03, "Cortex-A53" },
    { 0xd04, "Cortex-A35" },
    { 0xd05, "Cortex-A55" },
    { 0xd06, "Cortex-A65" },
    { 0xd07, "Cortex-A57" },
    { 0xd08, "Cortex-A72" },
    { 0xd09, "Cortex-A73" },
    { 0xd0a, "Cortex-A75" },
    { 0xd0b, "Cortex-A76" },
    { 0xd0c, "Neoverse-N1" },
    { 0xd0d, "Cortex-A77" },
    { 0xd0e, "Cortex-A76AE" },
    // --- Real-time R-profile: ARMv8-R specialized cores ---
    { 0xd13, "Cortex-R52" },
    { 0xd14, "Cortex-R82AE" },
    { 0xd15, "Cortex-R82" },
    { 0xd16, "Cortex-R52+" },
    // --- Microcontroller M-profile: ARMv8-M and ARMv8.1-M cores ---
    { 0xd20, "Cortex-M23" },
    { 0xd21, "Cortex-M33" },
    { 0xd22, "Cortex-M55" },
    { 0xd23, "Cortex-M85" },
    { 0xd24, "Cortex-M52" },
    // --- High-performance/Server: ARMv8.2-A+ and ARMv9-A scalable cores ---
    { 0xd40, "Neoverse-V1" },
    { 0xd41, "Cortex-A78" },
    { 0xd42, "Cortex-A78AE" },
    { 0xd43, "Cortex-A65AE" },
    { 0xd44, "Cortex-X1" },
    { 0xd46, "Cortex-A510" },
    { 0xd47, "Cortex-A710" },
    { 0xd48, "Cortex-X2" },
    { 0xd49, "Neoverse-N2" },
    { 0xd4a, "Neoverse-E1" },
    { 0xd4b, "Cortex-A78C" },
    { 0xd4c, "Cortex-X1C" },
    { 0xd4d, "Cortex-A715" },
    { 0xd4e, "Cortex-X3" },
    { 0xd4f, "Neoverse-V2" },
    // --- Latest generation: ARMv9.2-A and specialized efficiency designs ---
    { 0xd80, "Cortex-A520" },
    { 0xd81, "Cortex-A720" },
    { 0xd82, "Cortex-X4" },
    { 0xd83, "Neoverse-V3AE" },
    { 0xd84, "Neoverse-V3" },
    { 0xd85, "Cortex-X925" },
    { 0xd87, "Cortex-A725" },
    { 0xd88, "Cortex-A520AE" },
    { 0xd89, "Cortex-A720AE" },
    { 0xd8a, "Lumex-C1-Nano" },
    { 0xd8b, "Lumex-C1-Pro" },
    { 0xd8c, "Lumex-C1-Ultra" },
    { 0xd8e, "Neoverse-N3" },
    { 0xd8f, "Cortex-A320" },
    { 0xd90, "Lumex-C1-Premium" },
};

static constexpr ArmPartEntry kBrcmParts[] = {
    { 0x0f, "Brahma-B15" },
    { 0x100, "Brahma-B53" },
    { 0x516, "ThunderX2" },
};

static constexpr ArmPartEntry kDecParts[] = {
    { 0xa10, "SA110" },
    { 0xa11, "SA1100" },
};

static constexpr ArmPartEntry kCaviumParts[] = {
    { 0x0a0, "ThunderX" },
    { 0x0a1, "ThunderX-88XX" },
    { 0x0a2, "ThunderX-81XX" },
    { 0x0a3, "ThunderX-83XX" },
    { 0x0af, "ThunderX2-99xx" },
    { 0x0b0, "OcteonTX2" },
    { 0x0b1, "OcteonTX2-98XX" },
    { 0x0b2, "OcteonTX2-96XX" },
    { 0x0b3, "OcteonTX2-95XX" },
    { 0x0b4, "OcteonTX2-95XXN" },
    { 0x0b5, "OcteonTX2-95XXMM" },
    { 0x0b6, "OcteonTX2-95XXO" },
    { 0x0b8, "ThunderX3-T110" },
};

static constexpr ArmPartEntry kQcomParts[] = {
    { 0x001, "Oryon" },
    { 0x002, "Oryon-v2/v3" },
    { 0x00f, "Scorpion" },
    { 0x02d, "Scorpion" },
    { 0x04d, "Krait" },
    { 0x06f, "Krait" },
    { 0x201, "Kryo" },
    { 0x205, "Kryo" },
    { 0x211, "Kryo" },
    { 0x800, "Falkor-V1/Kryo" },
    { 0x801, "Kryo-V2" },
    { 0x802, "Kryo-3XX-Gold" },
    { 0x803, "Kryo-3XX-Silver" },
    { 0x804, "Kryo-4XX-Gold" },
    { 0x805, "Kryo-4XX-Silver" },
    { 0xc00, "Falkor" },
    { 0xc01, "Saphira" },
};

static constexpr ArmPartEntry kSamsungParts[] = {
    { 0x001, "Exynos-M1" },
    { 0x002, "Exynos-M3" },
    { 0x003, "Exynos-M4" },
    { 0x004, "Exynos-M5" },
};

static constexpr ArmPartEntry kNvidiaParts[] = {
    { 0x000, "Denver" },
    { 0x003, "Denver-2" },
    { 0x004, "Carmel" },
    { 0x010, "Olympus" },
    { 0xd4f, "Grace-C1" },
};

static constexpr ArmPartEntry kMarvellParts[] = {
    { 0x131, "Feroceon-88FR131" },
    { 0x581, "PJ4/PJ4b" },
    { 0x584, "PJ4B-MP" },
};

static constexpr ArmPartEntry kAppleParts[] = {
    // --- Legacy A-series: Swift through Thunder (A6 to A13 Bionic) ---
    { 0x000, "Swift" },
    { 0x001, "Cyclone" },
    { 0x002, "Typhoon" },
    { 0x003, "Typhoon-Capri" },
    { 0x004, "Twister" },
    { 0x005, "Twister-Elba-Malta" },
    { 0x006, "Hurricane" },
    { 0x007, "Hurricane-Myst" },
    { 0x008, "Monsoon" },
    { 0x009, "Mistral" },
    { 0x00b, "Vortex" },
    { 0x00c, "Tempest" },
    { 0x00f, "Tempest-M9" },
    { 0x010, "Vortex-Aruba" },
    { 0x011, "Tempest-Aruba" },
    { 0x012, "Lightning" },
    { 0x013, "Thunder" },
    { 0x026, "Thunder-M10" },
    // --- 5nm/4nm Era: A14/M1, A15/M2, and A16 Bionic architectures ---
    { 0x020, "Icestorm-A14" },
    { 0x021, "Firestorm-A14" },
    { 0x022, "Icestorm-M1" },
    { 0x023, "Firestorm-M1" },
    { 0x024, "Icestorm-M1-Pro" },
    { 0x025, "Firestorm-M1-Pro" },
    { 0x028, "Icestorm-M1-Max" },
    { 0x029, "Firestorm-M1-Max" },
    { 0x030, "Blizzard-A15" },
    { 0x031, "Avalanche-A15" },
    { 0x032, "Blizzard-M2" },
    { 0x033, "Avalanche-M2" },
    { 0x034, "Blizzard-M2-Pro" },
    { 0x035, "Avalanche-M2-Pro" },
    { 0x036, "Sawtooth-A16" },
    { 0x037, "Everest-A16" },
    { 0x038, "Blizzard-M2-Max" },
    { 0x039, "Avalanche-M2-Max" },
    // --- M3 Family: Ibiza/Lobos/Palma cores - TSMC 3nm Enhanced node ---
    { 0x040, "Sawtooth-A16" },
    { 0x041, "Everest-A16" }, // (Alt IDs seen in XNU)
    { 0x042, "M3-Efficiency" },
    { 0x043, "M3-Performance" },
    { 0x044, "M3-Pro-Efficiency" },
    { 0x045, "M3-Pro-Performance" },
    { 0x046, "Sawtooth-M11" },
    { 0x048, "M3-Max-Efficiency" },
    { 0x049, "M3-Max-Performance" },
    // --- A17 Pro: Coll core - First 3nm mobile architecture iteration ---
    { 0x050, "A17-Pro-Efficiency" },
    { 0x051, "A17-Pro-Performance" },
    // --- M4 Family: Donan/Brava cores - ARMv9.2-A latest generation ---
    { 0x052, "M4-Efficiency" },
    { 0x053, "M4-Performance" },
    { 0x054, "M4-Pro-Efficiency" },
    { 0x055, "M4-Pro-Performance" },
    { 0x058, "M4-Max-Efficiency" },
    { 0x059, "M4-Max-Performance" },
    // --- ARMv9.2-A Latest generation (2024-2026 architectures) ---
    { 0x060, "A18-Efficiency" },
    { 0x061, "A18-Performance" },
    { 0x062, "M5-Hidra (P-Core)" },
    { 0x063, "M5-Sotra (E-Core)" },
    { 0x064, "A18-Pro-Efficiency" },
    { 0x065, "A18-Pro-Performance" },
};

static constexpr ArmPartEntry kFaradayParts[] = {
    { 0x526, "FA526" },
    { 0x626, "FA626" },
};

static constexpr ArmPartEntry kIntelParts[] = {
    { 0x200, "i80200" },
    { 0x210, "PXA250A" },
    { 0x212, "PXA210A" },
    { 0x242, "i80321-400" },
    { 0x243, "i80321-600" },
    { 0x290, "PXA250B/PXA26x" },
    { 0x292, "PXA210B" },
    { 0x2c2, "i80321-400-B0" },
    { 0x2c3, "i80321-600-B0" },
    { 0x2d0, "PXA250C/PXA255" },
    { 0x2d2, "PXA210C" },
    { 0x411, "PXA27x" },
    { 0x41c, "IPX425-533" },
    { 0x41d, "IPX425-400" },
    { 0x41f, "IPX425-266" },
    { 0x682, "PXA32x" },
    { 0x683, "PXA930/PXA935" },
    { 0x688, "PXA30x" },
    { 0x689, "PXA31x" },
    { 0xb11, "SA1110" },
    { 0xc12, "IPX1200" },
};

static constexpr ArmPartEntry kFujitsuParts[] = {
    { 0x001, "A64FX" },
    { 0x003, "MONAKA" },
};

static constexpr ArmPartEntry kHisiParts[] = {
    { 0xd01, "TaiShan-v110" },
    { 0xd02, "TaiShan-v120" },
    { 0xd03, "TaiShan-v130" },
    { 0xd40, "Cortex-A76" },
    { 0xd41, "Cortex-A77" },
};

static constexpr ArmPartEntry kAmpereParts[] = {
    { 0xac3, "Ampere-1" },
    { 0xac4, "Ampere-1a" },
    { 0xac5, "Ampere-2" },
};

static constexpr ArmPartEntry kApmParts[] = {
    { 0x000, "X-Gene" },
};

static constexpr ArmPartEntry kGoogleParts[] = {
    { 0xd4f, "Axion" },
};

static constexpr ArmPartEntry kPhytiumParts[] = {
    { 0x303, "FTC310" },
    { 0x660, "FTC660" },
    { 0x661, "FTC661" },
    { 0x662, "FTC662" },
    { 0x663, "FTC663" },
    { 0x664, "FTC664" },
    { 0x862, "FTC862" },
};

static constexpr ArmPartEntry kMsParts[] = {
    { 0xd49, "Azure-Cobalt-100" },
    { 0xd84, "Azure-Cobalt-200" },
};


inline constexpr auto kArmImplementers = std::to_array<ArmImplEntry>({
    { 0x41, "ARM", kArmParts },
    { 0x42, "Broadcom", kBrcmParts },
    { 0x43, "Cavium", kCaviumParts },
    { 0x44, "DEC", kDecParts },
    { 0x46, "Fujitsu", kFujitsuParts },
    { 0x47, "Google", kGoogleParts },
    { 0x48, "HiSilicon", kHisiParts },
    { 0x49, "Infineon", {} },
    { 0x4d, "Motorola", {} },
    { 0x4e, "NVIDIA", kNvidiaParts },
    { 0x50, "APM", kApmParts },
    { 0x51, "Qualcomm", kQcomParts },
    { 0x53, "Samsung", kSamsungParts },
    { 0x56, "Marvell", kMarvellParts },
    { 0x61, "Apple", kAppleParts },
    { 0x66, "Faraday", kFaradayParts },
    { 0x69, "Intel", kIntelParts },
    { 0x6d, "Microsoft", kMsParts },
    { 0x70, "Phytium", kPhytiumParts },
    { 0xc0, "Ampere", kAmpereParts },
});

/**
 * @brief Maps an ARM implementer ID and part ID to a human-readable name.
 *
 * @param impl The CPU implementer ID (bits [31:24] of MIDR).
 * @param part The CPU part number (bits [15:4] of MIDR).
 * @return std::optional<std::string> The name if found, otherwise std::nullopt.
 */
inline constexpr auto lookup_arm_name = [](std::int32_t impl, std::int32_t part) -> std::optional<std::string> {
    auto impl_it = std::ranges::find_if(kArmImplementers, [impl](const auto& e) { return e.impl_id == impl; });
    if (impl_it == std::end(kArmImplementers)) { return std::nullopt; }

    auto part_it = std::ranges::find_if(impl_it->parts, [part](const auto& e) { return e.part_id == part; });

    if (part_it != impl_it->parts.end()) {
        // ARM Ltd cores → just the core name (e.g. "Cortex-A53")
        // Other vendors → prefix with vendor (e.g. "Qualcomm Oryon")
        if (impl == 0x41) { return std::string(part_it->name); }
        return std::format("{} {}", impl_it->vendor, part_it->name);
    }

    // Known vendor, unknown part → vendor + hex part
    return std::format("{} (part 0x{:03x})", impl_it->vendor, part);
};

/**
 * @brief Parses a hexadecimal value from /proc/cpuinfo given a specific key.
 *
 * @param cpuinfo The content of /proc/cpuinfo.
 * @param key The key to look for (e.g., "CPU implementer").
 * @return std::optional<int> The parsed integer value if successful.
 */
inline constexpr auto parse_cpuinfo_hex
    = [](const std::string& cpuinfo, std::string_view key) -> std::optional<std::int32_t> {
    const std::array<std::string_view, 1> keys = { key };
    const auto val_opt                         = lookup_info_field(cpuinfo, keys);
    if (!val_opt) { return std::nullopt; }

    auto val_str = *val_opt;
    if (string_utils::starts_with_ic<"0x">(val_str)) { val_str.remove_prefix(2); }
    std::int32_t val = 0;
    auto [ptr, ec]   = std::from_chars(val_str.data(), val_str.data() + val_str.size(), val, 16);
    return (ec == std::errc {}) ? std::optional { val } : std::nullopt;
};

/**
 * @brief Fallback mechanism to resolve ARM CPU name by reading MIDR_EL1 from sysfs.
 *
 * Requires kernel ≥ 4.11. Reads from /sys/devices/system/cpu/cpu0/regs/identification/midr_el1.
 *
 * @return std::optional<std::string> The resolved name if successful.
 */
inline constexpr auto parse_midr_sysfs = []() -> std::optional<std::string> {
    const std::string_view content = probe::get_midr_probe();
    if (content.empty()) { return std::nullopt; }

    auto val_str = trim_sv(content);
    if (string_utils::starts_with_ic<"0x">(val_str)) { val_str.remove_prefix(2); }

    std::uint64_t midr = 0;
    auto [ptr, ec]     = std::from_chars(val_str.data(), val_str.data() + val_str.size(), midr, 16);
    if (ec != std::errc {}) { return std::nullopt; }

    const std::int32_t impl = toInt((midr >> 24) & 0xFF);
    const std::int32_t part = toInt((midr >> 4) & 0xFFF);

    return lookup_arm_name(impl, part);
};

/**
 * @brief Primary resolver for ARM CPU model names.
 *
 * Tries parsing /proc/cpuinfo first, then falls back to sysfs MIDR_EL1.
 *
 * @return std::optional<std::string> The resolved CPU name.
 */
inline constexpr auto resolve_arm_model_name = []() -> std::optional<std::string> {
    ///< @brief Strategy 1: parse "CPU implementer" + "CPU part" from /proc/cpuinfo
    const auto& cpuinfo = probe::get_cpu_info_probe();
    if (!cpuinfo.empty()) {
        const auto impl_opt = parse_cpuinfo_hex(cpuinfo, "CPU implementer");
        const auto part_opt = parse_cpuinfo_hex(cpuinfo, "CPU part");
        if (impl_opt && part_opt) { return lookup_arm_name(*impl_opt, *part_opt); }
    }

    ///< @brief Strategy 2: read MIDR_EL1 register from sysfs (kernel ≥ 4.11)
    return parse_midr_sysfs();
};

} // namespace arm

/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "utils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <utility>
#include <numeric>
#include <ranges>
#include <span>
#include <type_traits>

namespace metrics {

/**
 * @brief High-precision latency histogram using logarithmic bucketing.
 *
 * This implementation provides high-resolution latency tracking with a constant relative error
 * across the entire measurement range. By using a 2432-bin distribution (38 groups of 64 bins),
 * it covers a wide dynamic range (~1.5 minutes at 3GHz) while remaining compact enough to fit
 * within a typical 32KB L1 Data Cache. This helps keep the structure cache-friendly to minimize
 * cache overhead during hot-path execution.
 */
class alignas(std::hardware_destructive_interference_size) LatencyHistogram final {
    static constexpr std::uint32_t kBinsPerGroup          = 64;
    static constexpr std::uint32_t kLog2BinsPerGroup      = 6;
    static constexpr std::uint32_t kGroups                = 38;
    static constexpr std::uint32_t kTotalBins             = kGroups * kBinsPerGroup;
    static constexpr std::uint64_t kMaxRepresentableValue = (1ULL << kGroups) - 1;

    static_assert(std::has_single_bit(kBinsPerGroup));
    static constexpr auto kWidths = [] {
        std::array<std::uint32_t, kGroups> widths {};
        for (std::uint32_t group = 0; group < kGroups; ++group) {
            widths[group] = group < kLog2BinsPerGroup ? (1u << group)
                : group == kLog2BinsPerGroup         ? kBinsPerGroup
                : group <= 17                       ? 75u
                                                    : 74u;
        }
        return widths;
    }();
    static constexpr auto kOffsets = [] {
        std::array<std::uint32_t, kGroups> offsets {};
        std::uint32_t sum {};
        for (std::uint32_t group = 0; group < kGroups; ++group) {
            offsets[group] = sum;
            sum += kWidths[group];
        }
        return offsets;
    }();
    static_assert(kOffsets.back() + kWidths.back() == kTotalBins);

    alignas(std::hardware_destructive_interference_size) std::array<std::uint64_t, kTotalBins> buckets_ {};

    alignas(std::hardware_destructive_interference_size) std::uint64_t count_ {};
    std::uint64_t total_cycles_ {};
    std::uint64_t min_cycles_ { std::numeric_limits<std::uint64_t>::max() };
    std::uint64_t max_cycles_ {};
    std::uint64_t overflow_count_ {};
    double mean_cycles_ {};
    double m2_cycles_ {};

    struct Bucket {
        std::uint32_t group {};
        std::uint32_t sub_bin {};
        std::uint64_t lower {};
        std::uint64_t step {};
    };

    [[nodiscard]] static constexpr Bucket locate(std::uint64_t cycles) noexcept {
        const auto val = std::clamp(cycles, 1ULL, kMaxRepresentableValue);
        const auto group = toUInt(std::bit_width(val)) - 1u;
        const std::uint64_t base = 1ULL << group;
        const auto width = kWidths[group];
        const auto sub = toUInt(((val - base) * width) / base);
        const auto lower = base + ((toULong(sub) * base + width - 1) / width);
        const auto upper = base + ((toULong(sub + 1u) * base + width - 1) / width) - 1ULL;
        return { group, sub, lower, upper - lower + 1ULL };
    }

    [[nodiscard]] constexpr auto& bucket(std::uint32_t group, std::uint32_t sub) noexcept {
        return buckets_[kOffsets[group] + sub];
    }

    void merge_moments(std::uint64_t additional_count, double additional_mean, double additional_m2) noexcept {
        if (additional_count == 0) { return; }
        if (count_ == 0) {
            mean_cycles_ = additional_mean;
            m2_cycles_ = additional_m2;
            count_ = additional_count;
            return;
        }
        const auto combined = safe_add(count_, additional_count).value_or(std::numeric_limits<std::uint64_t>::max());
        const double n1 = toDouble(count_);
        const double n2 = toDouble(additional_count);
        const double n = n1 + n2;
        const double delta = additional_mean - mean_cycles_;
        mean_cycles_ += delta * (n2 / n);
        m2_cycles_ += additional_m2 + delta * delta * (n1 / n) * n2;
        count_ = combined;
    }

public:
    LatencyHistogram() noexcept = default;

    /**
     * @brief Resets the histogram to its initial empty state.
     *
     * Allows reusing the same histogram instance across multiple benchmark runs
     * without reallocation overhead.
     */
    void reset() noexcept {
        buckets_.fill(0);
        count_        = 0;
        total_cycles_ = 0;
        min_cycles_   = std::numeric_limits<std::uint64_t>::max();
        max_cycles_   = 0;
        overflow_count_ = 0;
        mean_cycles_ = 0.0;
        m2_cycles_ = 0.0;
    }

    /**
     * @brief Records a latency measurement sample.
     *
     * Maps the measured duration (in cycles) to the appropriate logarithmic bin.
     * The mapping uses branch-minimized arithmetic to maintain deterministic
     * performance in high-throughput I/O paths.
     *
     * @param cycles The measured latency duration in CPU cycles.
     */
    void add(std::uint64_t cycles) noexcept {
        cycles = std::max<std::uint64_t>(cycles, 1);
        min_cycles_ = std::min(min_cycles_, cycles);
        max_cycles_ = std::max(max_cycles_, cycles);
        total_cycles_ += cycles;
        if (cycles > kMaxRepresentableValue) { ++overflow_count_; }

        const auto location = locate(cycles);
        ++bucket(location.group, location.sub_bin);
        merge_moments(1, toDouble(cycles), 0.0);
    }

    /**
     * @brief Records a sample with compensation for Coordinated Omission.
     *
     * When the system stalls, subsequent I/O requests are often delayed. Recording only
     * the actual completion time hides this "waiting" latency. This method adds virtual
     * samples for each expected interval that was missed, ensuring tail latency metrics
     * accurately reflect the true user-perceived performance during stalls.
     *
     * @param cycles The measured latency of the current I/O operation.
     * @param expected_interval_cycles The target time between I/O submissions.
     */
    void add_corrected(std::uint64_t cycles, std::uint64_t expected_interval_cycles) noexcept {
        add(cycles);
        if (expected_interval_cycles == 0 || cycles <= expected_interval_cycles) { return; }

        std::uint64_t missing = cycles - expected_interval_cycles;
        while (missing >= expected_interval_cycles) {
            const auto location = locate(missing);
            const auto limit = std::max(location.lower, expected_interval_cycles);
            const auto count = (missing - limit) / expected_interval_cycles + 1ULL;
            const auto last = missing - (count - 1ULL) * expected_interval_cycles;
            const long double n = static_cast<long double>(count);
            const long double first = static_cast<long double>(missing);
            const long double tail = static_cast<long double>(last);
            const long double interval = static_cast<long double>(expected_interval_cycles);
            const double batch_mean = static_cast<double>((first + tail) / 2.0L);
            const double batch_m2 = static_cast<double>((interval * interval) * n * (n * n - 1.0L) / 12.0L);
            const auto sum = static_cast<std::uint64_t>((first + tail) * n / 2.0L);

            bucket(location.group, location.sub_bin) += count;
            merge_moments(count, batch_mean, batch_m2);
            total_cycles_ += sum;
            min_cycles_ = std::min(min_cycles_, last);
            if (last > kMaxRepresentableValue) { overflow_count_ += count; }

            if (last <= expected_interval_cycles) { break; }
            missing = last - expected_interval_cycles;
        }
    }

    /**
     * @brief Accumulates data from another histogram into this instance.
     *
     * Enables parallel benchmark results to be aggregated into a single global view.
     * Uses C++23 zip views to ensure efficient, vectorized addition of buckets.
     *
     * @param other The histogram containing the samples to be merged.
     */
    void merge(const LatencyHistogram& other) noexcept {
        if (this == &other || other.count_ == 0) [[unlikely]] { return; }

        for (auto&& [mine, theirs] : std::views::zip(buckets_, other.buckets_)) {
            mine += theirs;
        }
        min_cycles_ = std::min(min_cycles_, other.min_cycles_);
        max_cycles_ = std::max(max_cycles_, other.max_cycles_);
        total_cycles_ += other.total_cycles_;
        overflow_count_ += other.overflow_count_;
        merge_moments(other.count_, other.mean_cycles_, other.m2_cycles_);
    }

    /**
     * @brief Retrieves the total number of samples recorded.
     * @return Total sample count across all bins.
     */
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

    /** @brief Retrieves the minimum observed latency in cycles. */
    [[nodiscard]] std::uint64_t min_cycles() const noexcept { return min_cycles_; }

    /** @brief Retrieves the maximum observed latency in cycles. */
    [[nodiscard]] std::uint64_t max_cycles() const noexcept { return max_cycles_; }

    /** @brief Retrieves the total sum of cycles recorded (for mean calculation). */
    [[nodiscard]] std::uint64_t total_cycles() const noexcept { return total_cycles_; }
    [[nodiscard]] std::uint64_t overflow_count() const noexcept { return overflow_count_; }
    [[nodiscard]] double mean_cycles() const noexcept { return mean_cycles_; }
    [[nodiscard]] double m2_cycles() const noexcept { return m2_cycles_; }

    /**
     * @brief Visits all buckets containing at least one sample.
     * @details Provides a clean Visitor Pattern to allow external analyzers to compute
     * metrics without needing to know the internal multidimensional layout or bucketing math.
     * @param func A callable accepting (std::uint64_t lower_bound, std::uint64_t step, std::uint64_t freq).
     */
    template <std::invocable<std::uint64_t, std::uint64_t, std::uint64_t> F>
    constexpr void for_each_active_bucket(F&& func) const
        noexcept(std::is_nothrow_invocable_v<F&, std::uint64_t, std::uint64_t, std::uint64_t>) {
        constexpr bool kCanShortCircuit
            = std::convertible_to<std::invoke_result_t<F&, std::uint64_t, std::uint64_t, std::uint64_t>, bool>;

        for (std::uint32_t group = 0; group < kGroups; ++group) {
            const std::uint64_t base = 1ULL << group;
            const auto width = kWidths[group];
            for (std::uint32_t sub = 0; sub < width; ++sub) {
                const auto freq = buckets_[kOffsets[group] + sub];
                if (freq == 0) { continue; }
                const auto lower = base + ((toULong(sub) * base + width - 1) / width);
                const auto upper = base + ((toULong(sub + 1u) * base + width - 1) / width) - 1ULL;
                if constexpr (kCanShortCircuit) {
                    if (!std::invoke(func, lower, upper - lower + 1ULL, freq)) { return; }
                } else {
                    std::invoke(func, lower, upper - lower + 1ULL, freq);
                }
            }
        }
    }
};

/**
 * @brief Analyzes latency histogram data and converts cycles to temporal metrics.
 *
 * Provides a clean boundary (Single Responsibility Principle) between data storage (LatencyHistogram)
 * and statistical analysis. Extracts metric computation out of the hot-path storage class.
 */
class LatencyAnalyzer final {
    const LatencyHistogram& hist_;
    const double cycles_to_ns_;

    /**
     * @brief Safely converts CPU cycles to sub-nanosecond floating-point duration.
     */
    [[nodiscard]] static std::chrono::duration<double, std::nano> to_nanoseconds(
        double cycles, double cycles_to_ns) noexcept {
        if (!std::isfinite(cycles_to_ns) || cycles_to_ns <= 0.0) [[unlikely]] {
            return std::chrono::duration<double, std::nano> { 0.0 };
        }
        return std::chrono::duration<double, std::nano> { cycles / cycles_to_ns };
    }

public:
    LatencyAnalyzer(const LatencyHistogram& hist, double cycles_to_ns) noexcept
        : hist_(hist)
        , cycles_to_ns_(cycles_to_ns) {}

    /**
     * @brief Retrieves the minimum observed latency.
     * @return The shortest duration recorded.
     */
    [[nodiscard]] std::chrono::duration<double, std::nano> min() const noexcept {
        if (hist_.count() == 0) { return std::chrono::duration<double, std::nano> { 0.0 }; }
        return to_nanoseconds(toDouble(hist_.min_cycles()), cycles_to_ns_);
    }

    /**
     * @brief Retrieves the maximum observed latency.
     * @return The longest duration recorded.
     */
    [[nodiscard]] std::chrono::duration<double, std::nano> max() const noexcept {
        if (hist_.count() == 0) { return std::chrono::duration<double, std::nano> { 0.0 }; }
        return to_nanoseconds(toDouble(hist_.max_cycles()), cycles_to_ns_);
    }

    /**
     * @brief Calculates the arithmetic mean latency.
     * @return The average latency as a nanosecond duration.
     */
    [[nodiscard]] std::chrono::duration<double, std::nano> avg() const noexcept {
        if (hist_.count() == 0 || !std::isfinite(cycles_to_ns_) || cycles_to_ns_ <= 0.0) [[unlikely]] {
            return std::chrono::duration<double, std::nano> { 0.0 };
        }
        const double avg_cycles = hist_.mean_cycles();
        return std::chrono::duration<double, std::nano> { avg_cycles / cycles_to_ns_ };
    }

    /**
     * @brief Estimates the latency at a given percentile.
     *
     * Uses weighted linear interpolation within the target bucket to provide a continuous
     * estimation of the latency distribution. This approach minimizes quantization errors
     * inherent in discrete bucketing.
     *
     * @param target_percentile The target percentile (0.0 - 100.0).
     * @return The estimated latency duration for the requested percentile.
     */
    [[nodiscard]] std::chrono::duration<double, std::nano> percentile(double target_percentile) const noexcept {
        if (hist_.count() == 0) { return std::chrono::duration<double, std::nano> { 0.0 }; }

        const double requested = std::isnan(target_percentile) ? 0.0 : std::clamp(target_percentile, 0.0, 100.0);
        if (requested <= 0.0) { return min(); }
        if (requested >= 100.0) { return max(); }

        const auto rank = std::max(1.0, std::ceil((requested / 100.0) * toDouble(hist_.count())));
        std::uint64_t cumulative {};
        double estimate = toDouble(hist_.max_cycles());

        hist_.for_each_active_bucket([&](auto lower, auto step, auto freq) -> bool {
            cumulative += freq;
            if (toDouble(cumulative) < rank) { return true; }
            const auto upper = lower + step - 1ULL;
            estimate = requested <= 50.0
                ? std::midpoint(toDouble(lower), toDouble(upper))
                : toDouble(upper);
            return false;
        });

        return to_nanoseconds(std::clamp(estimate, toDouble(hist_.min_cycles()), toDouble(hist_.max_cycles())),
            cycles_to_ns_);
    }

    /**
     * @brief Computes the Coefficient of Variation (CV = σ/μ) from bucket data.
     *
     * Iterates the bucket array to compute variance using the grouped-data formula:
     * σ² = Σ(fᵢ · (xᵢ - μ)²) / N, following HdrHistogram's getStdDeviation() pattern.
     * The CV is dimensionless, so no cycles_to_ns conversion is needed.
     *
     * Lower CV → tighter latency distribution → more stable benchmark run.
     *
     * @return CV as a non-negative double. Returns 0.0 when count < 2.
     */
    [[nodiscard]] double cv() const noexcept {
        if (hist_.count() < 2 || hist_.mean_cycles() <= 0.0) { return 0.0; }
        const double variance = std::max(0.0, hist_.m2_cycles() / toDouble(hist_.count()));
        return std::sqrt(variance) / hist_.mean_cycles();
    }
};

} // namespace metrics

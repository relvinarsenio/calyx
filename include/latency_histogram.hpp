/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "mdspan.hpp"
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
    static constexpr std::uint64_t kMaxRepresentableValue = (1ULL << kGroups) - 1;

    static_assert(std::has_single_bit(kBinsPerGroup), "kBinsPerGroup must be a power of two");
    static_assert((1u << kLog2BinsPerGroup) == kBinsPerGroup);

    /**
     * @brief Frequency counts for each latency bin.
     *
     * Aligned to a cache line boundary to ensure buckets start at a deterministic offset.
     */
    alignas(std::hardware_destructive_interference_size) std::array<std::uint64_t, kGroups * kBinsPerGroup> buckets_ {};

    /**
     * @brief Summary metrics and counters.
     *
     * Separated from the bucket array by a cache line boundary to reduce accidental
     * cache-line sharing when colocated instances are updated independently.
     */
    alignas(std::hardware_destructive_interference_size) std::uint64_t count_ {};
    std::uint64_t total_cycles_ {};
    std::uint64_t min_cycles_ { std::numeric_limits<std::uint64_t>::max() };
    std::uint64_t max_cycles_ {};

    /**
     * @brief Provides read-only multidimensional access to the internal bucket distribution.
     * @details Formalizes the (Groups x Bins) mapping into a first-class view to eliminate
     * manual arithmetic errors and improve maintainability without runtime overhead.
     * @return A 2D view of the frequency distribution.
     */
    [[nodiscard]] constexpr auto view() const noexcept {
        return stx::mdspan<const std::uint64_t, stx::extents<std::uint32_t, kGroups, kBinsPerGroup>,
            stx::layout_right> { buckets_.data() };
    }

    /**
     * @brief Provides mutable multidimensional access to the internal bucket distribution.
     * @return A 2D mdspan view (Groups x BinsPerGroup).
     */
    [[nodiscard]] constexpr auto view() noexcept {
        return stx::mdspan<std::uint64_t, stx::extents<std::uint32_t, kGroups, kBinsPerGroup>, stx::layout_right> {
            buckets_.data()
        };
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
        /**
         * @note Normalize 0 cycles to 1 to accommodate low-resolution timers (e.g. ARM CNTVCT_EL0).
         *       Otherwise, std::bit_width(0) - 1 will underflow.
         */
        cycles = std::max<std::uint64_t>(cycles, 1);

        min_cycles_ = std::min(min_cycles_, cycles);
        max_cycles_ = std::max(max_cycles_, cycles);
        total_cycles_ += cycles;
        count_++;

        const std::uint64_t val      = std::min<std::uint64_t>(cycles, kMaxRepresentableValue);
        const std::uint32_t msb      = toUInt(std::bit_width(val)) - 1;
        const std::uint32_t group    = msb;
        const std::int32_t raw_shift = toInt(group) - toInt(kLog2BinsPerGroup);
        const std::uint32_t shift    = toUInt(raw_shift & ~(raw_shift >> 31));
        const std::uint32_t sub_bin  = toUInt(((val ^ (1ULL << msb)) >> shift) & (kBinsPerGroup - 1));

        view()[group, sub_bin]++;
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
            struct BatchUpdate {
                std::uint32_t group;
                std::uint32_t sub_bin;
                std::uint64_t count;
                std::uint64_t last_val;
                std::uint64_t cycles_sum;
            };

            const auto batch = [missing, expected_interval_cycles]() -> BatchUpdate {
                const std::uint64_t val      = std::min<std::uint64_t>(missing, kMaxRepresentableValue);
                const std::uint32_t msb      = toUInt(std::bit_width(val)) - 1;
                const std::uint32_t group    = msb;
                const std::uint64_t base     = 1ULL << group;
                const std::int32_t raw_shift = toInt(group) - toInt(kLog2BinsPerGroup);
                const std::uint32_t shift    = toUInt(raw_shift & ~(raw_shift >> 31));

                const std::uint32_t sub_bin = toUInt(((val ^ base) >> shift) & (kBinsPerGroup - 1));
                const std::uint64_t step    = (group >= kLog2BinsPerGroup) ? (base >> kLog2BinsPerGroup) : 1;
                const std::uint64_t lower   = base + (toULong(sub_bin) * step);

                const std::uint64_t limit = std::max(lower, expected_interval_cycles);
                const std::uint64_t count = (missing - limit) / expected_interval_cycles + 1;

                const std::uint64_t last_val   = missing - (count - 1) * expected_interval_cycles;
                const std::uint64_t sum_term   = missing + last_val;
                const std::uint64_t cycles_sum = (count / 2) * sum_term + (count % 2) * (sum_term / 2);

                return { group, sub_bin, count, last_val, cycles_sum };
            }();

            view()[batch.group, batch.sub_bin] += batch.count;
            count_ = safe_add(count_, batch.count).value_or(std::numeric_limits<std::uint64_t>::max());
            total_cycles_ += batch.cycles_sum;
            min_cycles_ = std::min(min_cycles_, batch.last_val);

            if (batch.last_val <= expected_interval_cycles) { break; }
            missing = batch.last_val - expected_interval_cycles;
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
        count_ = safe_add(count_, other.count_).value_or(std::numeric_limits<std::uint64_t>::max());
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

        for (auto [group, sub_bin] :
            std::views::cartesian_product(std::views::iota(0u, kGroups), std::views::iota(0u, kBinsPerGroup))) {

            const std::uint64_t freq = view()[group, sub_bin];
            if (freq == 0) { continue; }

            const std::uint64_t base        = 1ULL << group;
            const std::uint64_t step        = (group >= kLog2BinsPerGroup) ? (base >> kLog2BinsPerGroup) : 1;
            const std::uint64_t lower_bound = base + (toULong(sub_bin) * step);

            if constexpr (kCanShortCircuit) {
                if (!std::invoke(func, lower_bound, step, freq)) { break; }
            } else {
                std::invoke(func, lower_bound, step, freq);
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
     * @brief Safely converts CPU cycles to std::chrono::nanoseconds without signed overflow.
     */
    [[nodiscard]] static std::chrono::nanoseconds to_nanoseconds(double cycles, double cycles_to_ns) noexcept {
        if (!std::isfinite(cycles_to_ns) || cycles_to_ns <= 0.0) [[unlikely]] { return std::chrono::nanoseconds { 0 }; }
        const double ns         = cycles / cycles_to_ns;
        constexpr double max_ns = toDouble(std::chrono::nanoseconds::max().count());
        if (ns >= max_ns) [[unlikely]] { return std::chrono::nanoseconds::max(); }
        return std::chrono::nanoseconds { static_cast<std::chrono::nanoseconds::rep>(ns) };
    }

public:
    LatencyAnalyzer(const LatencyHistogram& hist, double cycles_to_ns) noexcept
        : hist_(hist)
        , cycles_to_ns_(cycles_to_ns) {}

    /**
     * @brief Retrieves the minimum observed latency.
     * @return The shortest duration recorded.
     */
    [[nodiscard]] std::chrono::nanoseconds min() const noexcept {
        if (hist_.count() == 0) { return std::chrono::nanoseconds { 0 }; }
        return to_nanoseconds(toDouble(hist_.min_cycles()), cycles_to_ns_);
    }

    /**
     * @brief Retrieves the maximum observed latency.
     * @return The longest duration recorded.
     */
    [[nodiscard]] std::chrono::nanoseconds max() const noexcept {
        if (hist_.count() == 0) { return std::chrono::nanoseconds { 0 }; }
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
        const double avg_cycles = toDouble(hist_.total_cycles()) / toDouble(hist_.count());
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
    [[nodiscard]] std::chrono::nanoseconds percentile(double target_percentile) const noexcept {
        if (hist_.count() == 0) { return std::chrono::nanoseconds { 0 }; }

        const double requested = std::clamp(target_percentile, 0.0, 100.0);
        if (requested <= 0.0) { return min(); }
        if (requested >= 100.0) { return max(); }

        const auto target = (requested / 100.0) * toDouble(hist_.count());

        std::uint64_t cumulative {};
        auto result = max();

        hist_.for_each_active_bucket([target, cycles_to_ns = this->cycles_to_ns_, &cumulative, &result](
                                         const auto lower_bound, const auto step, const auto freq) -> bool {
            if (toDouble(cumulative + freq) < target) {
                cumulative += freq;
                return true; // continue iteration
            }

            if (step == 1) [[likely]] {
                result = to_nanoseconds(toDouble(lower_bound), cycles_to_ns);
            } else {
                const auto fraction     = (target - toDouble(cumulative)) / toDouble(freq);
                const auto interpolated = toDouble(lower_bound) + (fraction * toDouble(step - 1));
                result                  = to_nanoseconds(interpolated, cycles_to_ns);
            }
            return false; // break iteration
        });

        return result;
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
        if (hist_.count() < 2) { return 0.0; }

        const auto sample_count = toDouble(hist_.count());

        /** @note Pass 1: compute mean from bucket midpoints (consistent grouped-data mean). */
        double weighted_sum {};
        hist_.for_each_active_bucket([&weighted_sum](const auto lower_bound, const auto step, const auto freq) {
            const auto midpoint = toDouble(lower_bound) + toDouble(step - 1) * 0.5;
            weighted_sum += toDouble(freq) * midpoint;
        });

        const auto mean = weighted_sum / sample_count;
        if (mean <= 0.0) [[unlikely]] { return 0.0; }

        /** @note Pass 2: compute variance using midpoints to determine the stability of measured latencies. */
        double sum_sq_diff {};
        hist_.for_each_active_bucket([mean, &sum_sq_diff](const auto lower_bound, const auto step, const auto freq) {
            const auto midpoint = toDouble(lower_bound) + toDouble(step - 1) * 0.5;
            const auto diff     = midpoint - mean;
            sum_sq_diff += toDouble(freq) * diff * diff;
        });

        const auto stddev = std::sqrt(sum_sq_diff / sample_count);
        return stddev / mean;
    }
};

} // namespace metrics

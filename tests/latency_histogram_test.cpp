#include "latency_histogram.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

using metrics::LatencyAnalyzer;
using metrics::LatencyHistogram;

namespace {

void check_close(double actual, double expected, double tol = 1e-7) {
    assert(std::isfinite(actual));
    assert(std::abs(actual - expected) <= tol * std::max(1.0, std::abs(expected)));
}

double exact_cv(const std::vector<std::uint64_t>& values) {
    const long double n = static_cast<long double>(values.size());
    const long double total = std::accumulate(values.begin(), values.end(), 0.0L);
    const long double mean = total / n;
    long double m2 {};
    for (auto sample : values) {
        const long double delta = static_cast<long double>(sample) - mean;
        m2 += delta * delta;
    }
    return static_cast<double>(std::sqrt(m2 / n) / mean);
}

void test_empty_reset_and_constant() {
    LatencyHistogram hist {};
    LatencyAnalyzer empty { hist, 1.0 };
    assert(hist.count() == 0);
    check_close(empty.cv(), 0.0);
    check_close(empty.percentile(99.0).count(), 0.0);
    for (int i = 0; i < 100; ++i) { hist.add(1039); }
    LatencyAnalyzer analyzer { hist, 1.0 };
    assert(hist.count() == 100);
    check_close(analyzer.avg().count(), 1039.0);
    check_close(analyzer.cv(), 0.0);
    for (double p : { 0.0, 50.0, 95.0, 99.9, 100.0 }) {
        check_close(analyzer.percentile(p).count(), 1039.0);
    }
    hist.reset();
    assert(hist.count() == 0);
    assert(hist.overflow_count() == 0);
}

void test_quantile_invariants() {
    std::mt19937_64 rng { 0x51C5B6BULL };
    for (int iteration = 0; iteration < 120; ++iteration) {
        LatencyHistogram hist {};
        std::vector<std::uint64_t> raw {};
        for (int i = 0; i < 900; ++i) {
            const std::uint64_t cycles = 1 + (rng() % 250000ULL);
            hist.add(cycles);
            raw.push_back(cycles);
        }
        std::ranges::sort(raw);
        LatencyAnalyzer analyzer { hist, 1.0 };
        double prev = 0.0;
        for (double p : { 0.0, 1.0, 10.0, 50.0, 51.0, 90.0, 95.0, 99.0, 99.9, 100.0 }) {
            const double q = analyzer.percentile(p).count();
            assert(q >= static_cast<double>(raw.front()));
            assert(q <= static_cast<double>(raw.back()));
            assert(q >= prev);
            prev = q;
        }
        const auto expect_rank = (static_cast<std::size_t>(std::ceil(0.99 * raw.size())) - 1);
        const auto exact = raw[expect_rank];
        const auto approx = analyzer.percentile(99.0).count();
        assert(approx >= static_cast<double>(exact));
        check_close(analyzer.cv(), exact_cv(raw), 1e-6);
    }
}

void test_merge_moments() {
    LatencyHistogram first {};
    LatencyHistogram second {};
    LatencyHistogram reference {};
    std::vector<std::uint64_t> raw {};
    for (std::uint64_t i = 0; i < 20000; ++i) {
        const std::uint64_t cycles = 10 + ((i * 7919ULL) % 1000000ULL);
        (i % 2 == 0 ? first : second).add(cycles);
        reference.add(cycles);
        raw.push_back(cycles);
    }
    first.merge(second);
    assert(first.count() == reference.count());
    assert(first.total_cycles() == reference.total_cycles());
    check_close(first.mean_cycles(), reference.mean_cycles());
    check_close(LatencyAnalyzer { first, 1.0 }.cv(), exact_cv(raw), 1e-6);
    check_close(LatencyAnalyzer { first, 1.0 }.percentile(99.0).count(),
        LatencyAnalyzer { reference, 1.0 }.percentile(99.0).count());
    first.merge(first);
    assert(first.count() == reference.count());
}

void test_corrected() {
    for (std::uint64_t interval : { 1ULL, 7ULL, 33ULL, 500ULL, 8192ULL }) {
        for (std::uint64_t cycles : { 1024ULL, 4096ULL, 100000ULL, 250000ULL }) {
            LatencyHistogram batched {};
            LatencyHistogram reference {};
            batched.add_corrected(cycles, interval);
            reference.add(cycles);
            if (interval > 0 && cycles > interval) {
                auto missing = cycles - interval;
                while (missing >= interval) {
                    reference.add(missing);
                    missing -= interval;
                }
            }
            assert(batched.count() == reference.count());
            assert(batched.total_cycles() == reference.total_cycles());
            assert(batched.min_cycles() == reference.min_cycles());
            assert(batched.max_cycles() == reference.max_cycles());
            check_close(batched.mean_cycles(), reference.mean_cycles());
            check_close(LatencyAnalyzer { batched, 1.0 }.cv(),
                LatencyAnalyzer { reference, 1.0 }.cv(), 1e-6);
            check_close(LatencyAnalyzer { batched, 1.0 }.percentile(95.0).count(),
                LatencyAnalyzer { reference, 1.0 }.percentile(95.0).count());
        }
    }
}

void test_overflow() {
    LatencyHistogram hist {};
    hist.add(std::numeric_limits<std::uint64_t>::max());
    assert(hist.overflow_count() == 1);
    hist.add_corrected(1ULL << 39, 1ULL << 37);
    assert(hist.overflow_count() == 4);
    LatencyAnalyzer analyzer { hist, 1.0 };
    assert(analyzer.percentile(99.0).count() <= analyzer.max().count());
}

} // namespace

int main() {
    test_empty_reset_and_constant();
    test_quantile_invariants();
    test_merge_moments();
    test_corrected();
    test_overflow();
}

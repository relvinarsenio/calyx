/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <ranges>
#include <span>

namespace prng {

/**
 * @brief SplitMix64 PRNG used for seeding 256-bit state engines from a 64-bit seed.
 *
 * @details This is the standard recommended generator for initializing Xoshiro states.
 */
class SplitMix64 {
public:
    using result_type = std::uint64_t;
    constexpr explicit SplitMix64(std::uint64_t seed) noexcept
        : state_(seed) {}

    [[nodiscard]] constexpr result_type operator()() noexcept {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15);
        z               = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z               = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t state_ {};
};

/**
 * @brief 100% Standard Compliant Xoshiro256++ 1.0 implementation.
 *
 * @details Models a UniformRandomBitGenerator for full compatibility with <random>.
 * Implementation follows the original specification by Blackman and Vigna.
 */
class Xoshiro256PlusPlus {
public:
    using result_type                                 = std::uint64_t;
    static constexpr result_type kGoldenRatioFallback = 0x9e3779b97f4a7c15;
    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<result_type>::max(); }

    /**
     * @brief Construct engine from a 64-bit seed using SplitMix64.
     */
    constexpr explicit Xoshiro256PlusPlus(std::uint64_t seed) noexcept
        : state_ {} {
        SplitMix64 sm(seed);
        std::ranges::generate(state_, [&sm] { return sm(); });
    }

    /**
     * @brief Construct engine from explicit 256-bit state.
     * @details Guards against the degenerate all-zero state, which would cause the generator
     * to produce only zeros indefinitely. If an all-zero state is provided, it deterministically
     * falls back to the golden ratio fractional constant.
     */
    constexpr Xoshiro256PlusPlus(result_type s0, result_type s1, result_type s2, result_type s3) noexcept
        : state_ { s0, s1, s2, s3 } {
        if ((s0 | s1 | s2 | s3) == 0) { state_[0] = kGoldenRatioFallback; }
    }

    /**
     * @brief Generate next 64-bit random value.
     */
    [[nodiscard]] constexpr result_type operator()() noexcept {
        const result_type res = std::rotl(state_[0] + state_[3], 23) + state_[0];
        const result_type t   = state_[1] << 17;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = std::rotl(state_[3], 45);

        return res;
    }

    /**
     * @brief Advances the state by 2^128 steps.
     */
    constexpr void jump() noexcept {
        static constexpr std::array<result_type, 4> kJump
            = { 0x180ec6d33cfd0aba, 0xd5a61266f0c9392c, 0xa9582618e03fc9aa, 0x39abdc4529b1661c };
        jump_by_pattern(kJump);
    }

    /**
     * @brief Advances the state by 2^192 steps.
     */
    constexpr void long_jump() noexcept {
        static constexpr std::array<result_type, 4> kLongJump
            = { 0x76e15d3efefdcbbf, 0xc5004e441c522fb3, 0x77710069854ee241, 0x39109bb02acbe635 };
        jump_by_pattern(kLongJump);
    }

private:
    std::array<result_type, 4> state_ {};

    /**
     * @brief Core jump logic using C++23 ranges and views.
     */
    constexpr void jump_by_pattern(std::span<const result_type, 4> pattern) noexcept {
        std::array<result_type, 4> accumulator {};

        for (auto [word, bit] :
            std::views::cartesian_product(std::views::iota(0uz, 4uz), std::views::iota(0uz, 64uz))) {
            if (pattern[word] & (1ULL << bit)) {
                std::ranges::transform(accumulator, state_, accumulator.begin(), std::bit_xor<> {});
            }
            [[maybe_unused]] auto _ = operator()();
        }
        state_ = accumulator;
    }
};

static_assert(std::uniform_random_bit_generator<Xoshiro256PlusPlus>);

} // namespace prng

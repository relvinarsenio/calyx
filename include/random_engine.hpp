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
 * @details Standard recommended generator for initializing Xoshiro states.
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
 * @brief Xoshiro256++ 1.0 — Fast, general-purpose 256-bit PRNG.
 *
 * @details Implements the original specification by Blackman and Vigna.
 * Models the C++23 std::uniform_random_bit_generator concept.
 * Supports fixed jumps (jump, long_jump) and arbitrary-distance jumps
 * (jump_ce, jump_n) via GF(2) polynomial arithmetic over the characteristic
 * polynomial of the generator.
 */
class Xoshiro256PlusPlus {
public:
    using result_type                             = std::uint64_t;
    static constexpr result_type kDefaultSeedWord = 0x9e3779b97f4a7c15;
    [[nodiscard]] static constexpr result_type min() noexcept { return 0; }
    [[nodiscard]] static constexpr result_type max() noexcept { return std::numeric_limits<result_type>::max(); }

    /**
     * @brief Construct from a 64-bit seed via SplitMix64.
     */
    constexpr explicit Xoshiro256PlusPlus(std::uint64_t seed) noexcept
        : state_ {} {
        SplitMix64 sm(seed);
        std::ranges::generate(state_, [&sm] { return sm(); });
    }

    /**
     * @brief Construct from explicit 256-bit state.
     * @details Guards against the all-zero degenerate state by falling back
     * to the golden ratio constant.
     */
    constexpr Xoshiro256PlusPlus(result_type s0, result_type s1, result_type s2, result_type s3) noexcept
        : state_ { s0, s1, s2, s3 } {
        if ((s0 | s1 | s2 | s3) == 0) { state_[0] = kDefaultSeedWord; }
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
     * @brief Advance state by 2^128 steps using precomputed constants.
     */
    constexpr void jump() noexcept {
        static constexpr std::array<result_type, 4> kJump
            = { 0x180ec6d33cfd0aba, 0xd5a61266f0c9392c, 0xa9582618e03fc9aa, 0x39abdc4529b1661c };
        jump_apply(kJump);
    }

    /**
     * @brief Advance state by 2^192 steps using precomputed constants.
     */
    constexpr void long_jump() noexcept {
        static constexpr std::array<result_type, 4> kLongJump
            = { 0x76e15d3efefdcbbf, 0xc5004e441c522fb3, 0x77710069854ee241, 0x39109bb02acbe635 };
        jump_apply(kLongJump);
    }

    /**
     * @brief Advance state by exactly c * 2^e steps.
     *
     * @details Computes x^(c * 2^e) mod charpoly at runtime via square-and-multiply
     * in GF(2)[x]. jump_ce(1, 128) is equivalent to jump(); jump_ce(1, 192) to long_jump().
     * Cost is O(log c + e) ring multiplications — negligible against the stream that follows.
     *
     * @param c  Coefficient; c * 2^e must be less than the period (2^256 - 1).
     * @param e  Power-of-two exponent.
     */
    constexpr void jump_ce(result_type c, std::uint32_t e) noexcept { jump_apply(gf2_jumppoly_ce(c, e)); }

    /**
     * @brief Advance state by an arbitrary 256-bit distance n.
     *
     * @details n is a little-endian 256-bit integer: n[0] + n[1]*2^64 + n[2]*2^128 + n[3]*2^192.
     * Computes x^n mod charpoly via square-and-multiply, then applies the result to the state.
     *
     * @param n  Jump distance as a 256-bit little-endian packed integer.
     */
    constexpr void jump_n(const std::array<result_type, 4>& n) noexcept { jump_apply(gf2_jumppoly_n(n)); }

private:
    std::array<result_type, 4> state_ {};

    /** 256-bit polynomial in GF(2)[x] packed as four 64-bit words (little-endian bits). */
    using Poly = std::array<result_type, 4>;

    /**
     * @brief Characteristic polynomial of xoshiro256++.
     *
     * @details Coefficient k is bit (k & 63) of word (k >> 6); the leading x^256 term is implicit.
     * Source: prng.di.unimi.it/xoshiro256plusplus.c
     */
    static constexpr Poly kCharPoly
        = { 0x9d116f2bb0f0f001, 0x0280002bcefd1a5e, 0x04b4edcf26259f85, 0x0003c03c3f3ecb19 };

    /**
     * @brief Core accumulate-and-step loop shared by all jump variants.
     * @details Matches the jump_apply() logic from the reference C source (prng.di.unimi.it),
     * extended to also serve jump() and long_jump() by accepting any Poly by const-ref.
     */
    constexpr void jump_apply(const Poly& poly) noexcept {
        std::array<result_type, 4> accumulator {};

        for (auto [word, bit] :
            std::views::cartesian_product(std::views::iota(0uz, 4uz), std::views::iota(0uz, 64uz))) {
            if (poly[word] & (1ULL << bit)) {
                std::ranges::transform(accumulator, state_, accumulator.begin(), std::bit_xor<> {});
            }
            [[maybe_unused]] auto _ = operator()();
        }
        state_ = accumulator;
    }

    /**
     * @brief Multiply polynomial a by x modulo kCharPoly in place.
     * @details Since POLY_DEG=256 is a multiple of 64, the x^256 overflow lives entirely in carry.
     */
    static constexpr void gf2_mulx(Poly& a) noexcept {
        std::uint64_t carry {};
        for (auto i : std::views::iota(0uz, 4uz)) {
            const std::uint64_t next_carry = a[i] >> 63;
            a[i]                           = (a[i] << 1) | carry;
            carry                          = next_carry;
        }
        if (carry != 0) { std::ranges::transform(a, kCharPoly, a.begin(), std::bit_xor<> {}); }
    }

    /**
     * @brief Multiply polynomial a by b modulo kCharPoly in place.
     * @details Horner's method over the bits of a from most-significant to least-significant.
     */
    static constexpr void gf2_mulmod(Poly& a, const Poly& b) noexcept {
        Poly result {};
        for (auto k : std::views::iota(0uz, 256uz) | std::views::reverse) {
            gf2_mulx(result);
            if ((a[k >> 6] >> (k & 63uz)) & 1uz) {
                std::ranges::transform(result, b, result.begin(), std::bit_xor<> {});
            }
        }
        a = result;
    }

    /**
     * @brief Compute x^(c * 2^e) mod kCharPoly via square-and-multiply.
     * @details Square-and-multiply over 64 bits of c, then e additional squarings.
     */
    [[nodiscard]] static constexpr Poly gf2_jumppoly_ce(result_type c, std::uint32_t e) noexcept {
        Poly out {};
        out[0] = 1;

        for (auto k : std::views::iota(0uz, 64uz) | std::views::reverse) {
            gf2_mulmod(out, out);
            if ((c >> k) & 1u) { gf2_mulx(out); }
        }
        for ([[maybe_unused]] auto _ : std::views::iota(0u, e)) {
            gf2_mulmod(out, out);
        }

        return out;
    }

    /**
     * @brief Compute x^n mod kCharPoly via square-and-multiply over all 256 bits of n.
     * @details Bits of n are processed from the most-significant down to bit 0.
     */
    [[nodiscard]] static constexpr Poly gf2_jumppoly_n(const Poly& n) noexcept {
        Poly out {};
        out[0] = 1;

        for (auto k : std::views::iota(0uz, 256uz) | std::views::reverse) {
            gf2_mulmod(out, out);
            if ((n[k >> 6] >> (k & 63uz)) & 1uz) { gf2_mulx(out); }
        }

        return out;
    }
};

static_assert(std::uniform_random_bit_generator<Xoshiro256PlusPlus>);

} // namespace prng

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
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <ranges>

namespace prng {

/**
 * @brief SplitMix64 PRNG used for seeding 256-bit state engines from a 64-bit seed.
 *
 * @details Standard recommended generator for initializing Xoshiro states.
 */
class SplitMix64 {
    std::uint64_t state_ {};

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
};

/**
 * @brief GF(2) ring arithmetic for jump-polynomial computation.
 *
 * @details Implementation detail of the PRNG's jump functions not public API.
 * Ported from prng.di.unimi.it/f2x.c.
 */
namespace f2x_impl {

template <typename T> struct is_uint64_array : std::false_type {};

template <std::size_t N> struct is_uint64_array<std::array<std::uint64_t, N>> : std::true_type {};

/** @brief Constrains T to std::array<uint64_t, N> for some N. */
template <typename T>
concept CharPolyArray = is_uint64_array<T>::value;

/**
 * @brief GF(2)[x] / (kCharPoly) ring, parameterized on the characteristic polynomial itself.
 *
 * @details CharPoly is a non-type template parameter so word count and poly_deg are
 * derived from it via .size(), never hardcoded. The requires clause enforces a
 * necessary condition for validity: the constant term must be 1, i.e. x=0 must not
 * be a root. Full irreducibility of the supplied constant is assumed already
 * established (Blackman & Vigna, Section 6).
 */
template <auto CharPoly>
    requires CharPolyArray<decltype(CharPoly)> && (CharPoly.size() > 0) && ((CharPoly[0] & std::uint64_t { 1 }) != 0)
struct GF2Ring {
    using Poly = decltype(CharPoly);

    static constexpr std::size_t kWords   = CharPoly.size();
    static constexpr std::size_t kPolyDeg = kWords * 64;
    static constexpr Poly kCharPoly       = CharPoly;

    /** @brief Multiplies a by x modulo kCharPoly, in place. */
    static constexpr void mulx(Poly& a) noexcept {
        std::uint64_t carry {};
        for (auto i : std::views::iota(std::size_t { 0 }, kWords)) {
            const std::uint64_t next_carry = a[i] >> 63;
            a[i]                           = (a[i] << 1) | carry;
            carry                          = next_carry;
        }
        if (carry != 0) { std::ranges::transform(a, kCharPoly, a.begin(), std::bit_xor<> {}); }
    }

    /** @brief Multiplies a by b modulo kCharPoly, in place, via Horner's method over the bits of a. */
    static constexpr void mulmod(Poly& a, const Poly& b) noexcept {
        Poly result {};
        for (auto k : std::views::iota(std::size_t { 0 }, kPolyDeg) | std::views::reverse) {
            mulx(result);
            if ((a[k >> 6] >> (k & 63uz)) & 1uz) {
                std::ranges::transform(result, b, result.begin(), std::bit_xor<> {});
            }
        }
        a = result;
    }

    /** @brief Computes x^(c * 2^e) mod kCharPoly via square-and-multiply. */
    [[nodiscard]] static constexpr Poly jumppoly_ce(std::uint64_t c, std::uint32_t e) noexcept {
        Poly out {};
        out[0] = 1;

        for (auto k : std::views::iota(std::size_t { 0 }, std::size_t { 64 }) | std::views::reverse) {
            mulmod(out, out);
            if ((c >> k) & std::uint64_t { 1 }) { mulx(out); }
        }
        for ([[maybe_unused]] auto _ : std::views::iota(std::uint32_t { 0 }, e)) {
            mulmod(out, out);
        }

        return out;
    }

    /** @brief Computes x^n mod kCharPoly via square-and-multiply over all kPolyDeg bits of n. */
    [[nodiscard]] static constexpr Poly jumppoly_n(const Poly& n) noexcept {
        Poly out {};
        out[0] = 1;

        for (auto k : std::views::iota(std::size_t { 0 }, kPolyDeg) | std::views::reverse) {
            mulmod(out, out);
            if ((n[k >> 6] >> (k & 63uz)) & 1uz) { mulx(out); }
        }

        return out;
    }
};

} // namespace f2x_impl

/**
 * @brief Xoshiro256++ 1.0 — 256-bit PRNG by Blackman & Vigna.
 *
 * @details A general-purpose, non-cryptographic PRNG suitable for parallel and
 * statistical workloads. Satisfies C++20 UniformRandomBitGenerator. Full C++23 required.
 * Supports fixed (jump, long_jump) and arbitrary-distance jumps (jump_ce, jump_n)
 * to partition the period for independent parallel streams without overlap.
 */
class Xoshiro256PlusPlus {
    std::array<std::uint64_t, 4> state_ {};

    /**
     * @brief Characteristic polynomial of xoshiro256++.
     *
     * @details Coefficients packed as four 64-bit words (little-endian bits);
     * the leading x^256 term is implicit. Source: prng.di.unimi.it/xoshiro256plusplus.c
     */
    static constexpr std::array<std::uint64_t, 4> kCharPoly
        = { 0x9d116f2bb0f0f001, 0x0280002bcefd1a5e, 0x04b4edcf26259f85, 0x0003c03c3f3ecb19 };

    /** @brief GF(2) ring bound to the characteristic polynomial of xoshiro256++. */
    using GF2 = f2x_impl::GF2Ring<kCharPoly>;

    using Poly = GF2::Poly;

    /**
     * @brief Core accumulate-and-step loop shared by all jump variants.
     * @details Matches the jump_apply() logic from the reference C source (prng.di.unimi.it),
     * extended to also serve jump() and long_jump() by accepting any Poly by const-ref.
     */
    constexpr void jump_apply(const Poly& poly) noexcept {
        std::array<result_type, GF2::kWords> accumulator {};

        const auto apply_word = [&accumulator, this](const result_type word) constexpr noexcept {
            [&accumulator, this, word]<std::size_t... Bits>(std::index_sequence<Bits...>) {
                const auto step_bit = [&accumulator, this, word]<std::size_t Bit>() constexpr noexcept {
                    if (word & (result_type { 1 } << Bit)) {
                        [&accumulator, this]<std::size_t... Ws>(std::index_sequence<Ws...>) {
                            ((accumulator[Ws] ^= state_[Ws]), ...);
                        }(std::make_index_sequence<GF2::kWords> {});
                    }
                    operator()();
                };
                (step_bit.template operator()<Bits>(), ...);
            }(std::make_index_sequence<64> {});
        };

        [&apply_word, &poly]<std::size_t... Is>(std::index_sequence<Is...>) { (apply_word(poly[Is]), ...); }(
            std::make_index_sequence<GF2::kWords> {});

        state_ = accumulator;
    }

    /** @brief Fills state_ by running SplitMix64 from the given seed. */
    constexpr void seed_from(std::uint64_t seed) noexcept {
        SplitMix64 sm(seed);
        std::ranges::generate(state_, [&sm] { return sm(); });
    }

public:
    using result_type = std::uint64_t;
    /** @brief 2^64 / φ (golden ratio), widely used as a well-distributed increment. */
    static constexpr result_type kDefaultSeedWord = 0x9e3779b97f4a7c15;
    [[nodiscard]] static constexpr result_type min() noexcept { return 0; }
    [[nodiscard]] static constexpr result_type max() noexcept { return std::numeric_limits<result_type>::max(); }

    /**
     * @brief Default constructor. Seeds via kDefaultSeedWord.
     *
     * @details Prevents the implicit value-initialization of state_ to all-zeros,
     * which would produce a degenerate generator that emits zero forever.
     */
    constexpr Xoshiro256PlusPlus() noexcept
        : Xoshiro256PlusPlus(kDefaultSeedWord) {}

    /**
     * @brief Construct from a 64-bit seed via SplitMix64.
     */
    constexpr explicit Xoshiro256PlusPlus(std::uint64_t seed) noexcept
        : state_ {} {
        seed_from(seed);
    }

    /**
     * @brief Construct from explicit 256-bit state.
     * @details Guards against the all-zero degenerate state — the fixed point of
     * xoshiro256++ where operator()() would emit zero indefinitely.
     */
    constexpr explicit Xoshiro256PlusPlus(result_type s0, result_type s1, result_type s2, result_type s3) noexcept
        : state_ { s0, s1, s2, s3 } {
        if ((s0 | s1 | s2 | s3) == 0) { seed_from(kDefaultSeedWord); }
    }

    /**
     * @brief Generate next 64-bit random value.
     */
    constexpr result_type operator()() noexcept {
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
     * @details Evaluates x^(c * 2^e) mod charpoly in GF(2)[x].
     * jump_ce(1, 128) ≡ jump(); jump_ce(1, 192) ≡ long_jump(). Cost: O(64 + e)
     * multiplications. Keep e ≤ 256, large values will block.
     *
     * @param c  Coefficient of the jump distance.
     * @param e  Power-of-two exponent. Cost is O(e). Keep small (≤ 256) to avoid blocking.
     */
    constexpr void jump_ce(result_type c, std::uint32_t e) noexcept { jump_apply(GF2::jumppoly_ce(c, e)); }

    /**
     * @brief Advance state by an arbitrary 256-bit distance n.
     *
     * @details n is a little-endian 256-bit integer: n[0] + n[1]*2^64 + n[2]*2^128 + n[3]*2^192.
     * The result is the linear transformation equivalent to exactly n steps of operator()().
     *
     * @param n  Jump distance as a 256-bit little-endian packed integer.
     */
    constexpr void jump_n(const Poly& n) noexcept { jump_apply(GF2::jumppoly_n(n)); }
};

static_assert(std::uniform_random_bit_generator<Xoshiro256PlusPlus>);

} // namespace prng

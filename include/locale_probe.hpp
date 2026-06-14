/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "numeric_cast.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <langinfo.h>
#include <optional>
#include <string_view>

namespace ui::locale::probe_impl {

/**
 * @brief Evaluates whether a character is equal to a known lowercase ASCII character,
 *        performing case-insensitive comparison on the left character.
 *
 * @note This comparator is asymmetric: the right-hand operand (rhs) MUST be
 *       already lowercase ASCII for the comparison to be correct.
 */
struct AsciiLowerEqualRight {
    [[nodiscard]] constexpr bool operator()(char lhs, char rhs) const noexcept {
        return static_cast<unsigned char>(std::tolower(toUChar(lhs))) == toUChar(rhs);
    }
};

template <std::size_t N> struct FixedString {
    std::array<char, N> chars {};
    consteval FixedString(const char (&str)[N]) { std::ranges::copy(str, chars.begin()); }
    [[nodiscard]] consteval std::size_t size() const noexcept { return N - 1uz; }
};

template <FixedString Pattern> struct StringMatcher {
    [[nodiscard]] static constexpr bool match(std::string_view str) noexcept {
        const std::string_view pattern { Pattern.chars.begin(), Pattern.chars.begin() + Pattern.size() };
        return std::ranges::contains_subrange(str, pattern, AsciiLowerEqualRight {});
    }
};

[[nodiscard]] constexpr bool is_utf8_encoding(std::string_view val) noexcept {
    return StringMatcher<"utf-8">::match(val) || StringMatcher<"utf8">::match(val);
}

/**
 * @brief Returns the value of an environment variable, or nullopt if unset/empty.
 *
 * Guards against both a missing key (nullptr) and an explicitly empty value,
 * both of which carry no locale information.
 */
[[nodiscard]] inline std::optional<std::string_view> get_env_sv(const char* name) noexcept {
    const char* val = std::getenv(name);
    if (val == nullptr || *val == '\0') { return std::nullopt; }
    return std::string_view { val };
}

/**
 * @brief Checks the system codeset (nl_langinfo) for UTF-8 declaration.
 *
 * Preferred over environment variables because it reflects the active locale
 * after any setlocale() calls, not just the inherited shell environment.
 */
[[nodiscard]] inline bool detect_from_codeset() noexcept {
    const char* codeset = nl_langinfo(CODESET);
    return codeset != nullptr && is_utf8_encoding(std::string_view { codeset });
}

/**
 * @brief Checks locale environment variables for UTF-8 in POSIX precedence order.
 *
 * Fallback when the codeset is unavailable or inconclusive. POSIX specifies
 * LC_ALL overrides LC_CTYPE, which overrides LANG.
 */
[[nodiscard]] inline bool detect_from_env() noexcept {
    return get_env_sv("LC_ALL")
        .or_else([] { return get_env_sv("LC_CTYPE"); })
        .or_else([] { return get_env_sv("LANG"); })
        .transform(is_utf8_encoding)
        .value_or(false);
}

} // namespace ui::locale::probe_impl

namespace ui::locale {

/**
 * @brief Returns true if the current environment advertises UTF-8 encoding.
 *
 * Result is cached after first call — locale does not change mid-process.
 */
[[nodiscard]] inline bool supports_utf8() noexcept {
    static const bool supported = probe_impl::detect_from_codeset() || probe_impl::detect_from_env();
    return supported;
}

} // namespace ui::locale

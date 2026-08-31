/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "color.hpp"
#include "config.hpp"
#include "file_descriptor.hpp"
#include "numeric_cast.hpp"
#include "posix.hpp"
#include "posix_error.hpp"
#include "random_engine.hpp"
#include "scope.hpp"
#include "tsc.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

/**
 * @brief Overflow-checked arithmetic core engine using Higher-Order Functions.
 *
 * The operation is injected at compile time via a stateless lambda expression.
 * No runtime branching occurs for the operation itself — only the mandatory overflow sentinel check remains.
 */
template <typename T1, typename T2> using wider_type_t = std::conditional_t<(sizeof(T1) >= sizeof(T2)), T1, T2>;
template <cast::standard_integer_type T1, cast::standard_integer_type T2, typename Func>
    requires(std::is_signed_v<T1> == std::is_signed_v<T2>)
    && std::invocable<Func, wider_type_t<T1, T2>, wider_type_t<T1, T2>, wider_type_t<T1, T2>&>
[[nodiscard]] constexpr auto safe_arith(T1 lhs, T2 rhs, Func&& op) noexcept -> std::optional<wider_type_t<T1, T2>> {
    using Common = wider_type_t<T1, T2>;
    Common result {};

    const bool is_overflow
        = std::forward<Func>(op)(cast::saturate_cast<Common>(lhs), cast::saturate_cast<Common>(rhs), result);
    return is_overflow ? std::nullopt : std::make_optional(result);
}

/** @brief Safely multiply two integer values with overflow checking. */
template <cast::standard_integer_type T1, cast::standard_integer_type T2>
[[nodiscard]] constexpr auto safe_mul(T1 lhs, T2 rhs) noexcept {
    return safe_arith(lhs, rhs, [](auto a, auto b, auto& out) { return __builtin_mul_overflow(a, b, &out); });
}

/** @brief Safely add two integer values with overflow checking. */
template <cast::standard_integer_type T1, cast::standard_integer_type T2>
[[nodiscard]] constexpr auto safe_add(T1 lhs, T2 rhs) noexcept {
    return safe_arith(lhs, rhs, [](auto a, auto b, auto& out) { return __builtin_add_overflow(a, b, &out); });
}

/** @brief Safely subtract two integer values with overflow checking. */
template <cast::standard_integer_type T1, cast::standard_integer_type T2>
[[nodiscard]] constexpr auto safe_sub(T1 lhs, T2 rhs) noexcept {
    return safe_arith(lhs, rhs, [](auto a, auto b, auto& out) { return __builtin_sub_overflow(a, b, &out); });
}

/**
 * @brief Checks if a value equals any of the specified template constants.
 *
 * Dispatched at compile time using fold expressions.
 */
template <auto... Vals, class T>
    requires(std::equality_comparable_with<T, decltype(Vals)> && ...)
[[nodiscard]] constexpr bool is_one_of(const T& val) noexcept {
    return ((val == Vals) || ...);
}

/**
 * @brief Idiomatic overload set generator for exhaustive std::variant matching.
 *
 * Inherits from multiple lambdas to create a unified visitation object.
 * @note Explicit deduction guides are omitted in favor of C++20 aggregate CTAD.
 */
template <class... Ts>
    requires(std::is_class_v<Ts> && ...)
struct overloaded : Ts... {
    using Ts::operator()...;
};

namespace fs = std::filesystem;

/**
 * @brief Removes surrounding quotes (double or single) from a string view.
 */
inline constexpr auto unquote = [](std::string_view input_view) noexcept -> std::string_view {
    if (input_view.size() < 2) { return input_view; }

    const bool double_quoted = input_view.front() == '"' && input_view.back() == '"';
    const bool single_quoted = input_view.front() == '\'' && input_view.back() == '\'';
    if (double_quoted || single_quoted) { return input_view.substr(1, input_view.size() - 2); }
    return input_view;
};

inline constexpr auto print_error
    = [](std::string_view message) noexcept { std::print(stderr, "{}{}{}\n", color::kRed, message, color::kReset); };

inline constexpr auto print_warning
    = [](std::string_view message) noexcept { std::print(stderr, "{}{}{}\n", color::kYellow, message, color::kReset); };

/**
 * @brief Detects the current terminal width in columns.
 */
inline constexpr auto get_term_width = []() noexcept -> std::size_t {
    struct winsize win_size;
    if (posix::file_descriptor::ioctl_raw(posix::file_descriptor::stdout_fd, TIOCGWINSZ, win_size)
        && win_size.ws_col > 0) {
        return std::min(toSize(win_size.ws_col), config::kTermWidth);
    }
    return config::kTermWidth;
};

inline constexpr auto print_line = []() noexcept {
    using namespace std::string_view_literals;
    const std::size_t width = get_term_width();
    std::println("{}", std::views::repeat("\u2500"sv, width) | std::views::join | std::ranges::to<std::string>());
};

inline constexpr auto print_centered_header = [](std::string_view text) noexcept {
    using namespace std::string_view_literals;
    const std::size_t width    = get_term_width();
    const std::size_t text_len = text.length();

    if (text_len >= width - 2) {
        std::println("{}", text);
        return;
    }

    const std::size_t remaining = width - text_len - 2;
    const std::size_t left_pad  = remaining / 2;
    const std::size_t right_pad = remaining - left_pad;

    const auto left_line = std::views::repeat("\u2500"sv, left_pad) | std::views::join | std::ranges::to<std::string>();
    const auto right_line
        = std::views::repeat("\u2500"sv, right_pad) | std::views::join | std::ranges::to<std::string>();

    std::println("{} {} {}", left_line, text, right_line);
};

[[nodiscard]] constexpr std::string_view trim_sv(std::convertible_to<std::string_view> auto&& str) noexcept {
    const std::string_view sv { std::forward<decltype(str)>(str) };
    constexpr auto is_space = [](char const ch) noexcept {
        return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '\v' || ch == '\f';
    };
    auto first = std::ranges::find_if_not(sv, is_space);
    if (first == sv.end()) { return {}; }
    auto last = std::ranges::find_if_not(sv | std::views::reverse, is_space).base();
    return std::string_view(first, last);
}

[[nodiscard]] constexpr std::string trim(std::convertible_to<std::string_view> auto&& str) {
    return std::string { trim_sv(std::forward<decltype(str)>(str)) };
}

namespace string_utils {

/**
 * @brief Case-insensitive equality between two runtime strings.
 *
 * Symmetric: both sides are lowercased before comparison.
 */
[[nodiscard]] constexpr bool equals_ic(
    std::convertible_to<std::string_view> auto&& lhs, std::convertible_to<std::string_view> auto&& rhs) noexcept {
    const std::string_view sv1 { std::forward<decltype(lhs)>(lhs) };
    const std::string_view sv2 { std::forward<decltype(rhs)>(rhs) };
    return sv1.size() == sv2.size() && std::ranges::equal(sv1, sv2, [](char a, char b) noexcept {
        return std::tolower(toUChar(a)) == std::tolower(toUChar(b));
    });
}

/**
 * @brief Evaluates whether a character is equal to a known lowercase ASCII character,
 *        performing case-insensitive comparison on the left character.
 *
 * @note This comparator is asymmetric: the right-hand operand (rhs) MUST be
 *       already lowercase ASCII for the comparison to be correct.
 */
struct AsciiLowerEqualRight {
    [[nodiscard]] constexpr bool operator()(char lhs, char rhs) const noexcept {
        const unsigned char lhs_ch = toUChar(lhs);
        const unsigned char rhs_ch = toUChar(rhs);
        return std::tolower(lhs_ch) == rhs_ch;
    }
};

template <std::size_t N> struct FixedString {
    std::array<char, N> chars {};
    consteval FixedString(const char (&str)[N]) { std::ranges::copy(str, chars.begin()); }
    [[nodiscard]] consteval std::size_t size() const noexcept { return N - 1uz; }
};

template <FixedString Pattern> [[nodiscard]] constexpr bool contains_ic(std::string_view str) noexcept {
    static constexpr auto kPattern = Pattern;
    static constexpr std::string_view pattern { kPattern.chars.begin(), kPattern.size() };
    return std::ranges::contains_subrange(str, pattern, AsciiLowerEqualRight {});
}

template <FixedString Pattern> [[nodiscard]] constexpr bool starts_with_ic(std::string_view str) noexcept {
    static constexpr auto kPattern = Pattern;
    if (str.size() < kPattern.size()) { return false; }
    static constexpr std::string_view prefix { kPattern.chars.begin(), kPattern.size() };
    return std::ranges::equal(str.substr(0, kPattern.size()), prefix, AsciiLowerEqualRight {});
}

/**
 * @brief Case-insensitive exact matcher against a compile-time pattern.
 *
 * Completes the matcher family alongside @ref contains_ic (substring)
 * and @ref starts_with_ic (prefix).
 *
 * @note Asymmetric: the Pattern MUST be all-lowercase for correct matching.
 */
template <FixedString Pattern> [[nodiscard]] constexpr bool equals_ic(std::string_view str) noexcept {
    static constexpr auto kPattern = Pattern;
    if (str.size() != kPattern.size()) { return false; }
    static constexpr std::string_view pattern { kPattern.chars.begin(), kPattern.size() };
    return std::ranges::equal(str, pattern, AsciiLowerEqualRight {});
}

[[nodiscard]] constexpr std::string_view strip_bracketed_prefix(std::string_view sv) noexcept {
    return std::optional { sv }
        .and_then([](auto str) { return str.starts_with('[') ? std::optional { str } : std::nullopt; })
        .and_then([](auto str) {
            const auto close = str.find(']');
            return (close != std::string_view::npos)
                ? std::optional { std::pair { str.substr(0, close).substr(1), str.substr(close).substr(1) } }
                : std::optional<std::pair<std::string_view, std::string_view>> {};
        })
        .and_then([](auto&& pair) {
            const auto [inside, rest]     = pair;
            constexpr auto is_ascii_digit = [](char ch) noexcept { return ch >= '0' && ch <= '9'; };
            return !inside.empty() && std::ranges::all_of(inside, is_ascii_digit) ? std::optional { pair }
                                                                                  : std::nullopt;
        })
        .transform([](auto&& pair) { return trim_sv(pair.second); })
        .value_or(sv);
}

[[nodiscard]] constexpr std::string_view strip_brackets(std::string_view sv) noexcept {
    return std::optional { sv }
        .and_then([](auto str) {
            return (str.starts_with('[') && str.ends_with(']')) ? std::optional { str } : std::nullopt;
        })
        .transform([](auto str) {
            str.remove_prefix(1uz);
            str.remove_suffix(1uz);
            return str;
        })
        .value_or(sv);
}

} // namespace string_utils

/**
 * @brief Truncates a string to a maximum length, appending an ellipsis if needed.
 */
inline constexpr auto truncate_error
    = [](std::string_view message, std::size_t max_len = config::kMaxErrorDisplayLen) -> std::string {
    if (message.length() <= max_len) { return std::string(message); }
    const auto limit = (max_len > 3) ? max_len - 3 : 0;
    return std::format("{:.{}}...", message, limit);
};

/**
 * @brief Retrieves the system's page size in bytes.
 */
inline constexpr auto get_page_size = []() noexcept -> std::uint64_t {
    static const std::uint64_t size = []() {
        const auto res = posix::expect_result<posix::error_style::posix>(::sysconf(_SC_PAGESIZE));
        return (res && *res > 0) ? toULong(*res) : 4096ULL;
    }();
    return size;
};

namespace format_impl {

/**
 * @brief Aggregates the mutable state of scaled formatting variables.
 * @details Grouped into a single structure to avoid passing multiple output references
 *          in formatting helper signatures.
 */
struct format_state {
    double scaled_value {};
    std::size_t suffix_index {};
};

inline constexpr auto adjust_overflow
    = [](format_state state, double base, std::size_t max_suffixes) noexcept -> format_state {
    const double rounded = std::round(state.scaled_value * 100.0) / 100.0;
    const auto candidate = safe_add(state.suffix_index, 1uz);
    const bool overflow  = (rounded >= base) && (candidate.value_or(0uz) < max_suffixes);

    const std::array kNextIndices = { state.suffix_index, candidate.value_or(state.suffix_index) };
    const std::array kScaleValues = { rounded, rounded / base };

    const double next_scaled_value = kScaleValues[overflow];
    const double next_rounded      = std::round(next_scaled_value * 100.0) / 100.0;

    return { next_rounded, kNextIndices[overflow] };
};

} // namespace format_impl

/**
 * @brief Formats a byte count into a human-readable string (e.g., "1.2 MB").
 * @details Evaluates the scale index and dynamic precision in a single allocation pass
 *          without string mutations or float subtraction instability.
 */
inline constexpr auto format_bytes = [](std::uint64_t bytes) -> std::string {
    static constexpr std::array kSuffixes = { "B", "KB", "MB", "GB", "TB" };

    const std::size_t bits         = toSize(std::bit_width(bytes));
    const std::size_t shift        = safe_sub(bits, 1uz).value_or(0uz);
    const std::size_t suffix_index = std::min<std::size_t>(shift / 10uz, kSuffixes.size() - 1uz);
    const double scaled_value      = toDouble(bytes) / toDouble(1ULL << safe_mul(suffix_index, 10uz).value_or(0uz));

    const auto state = format_impl::adjust_overflow(
        format_impl::format_state { scaled_value, suffix_index }, 1024.0, kSuffixes.size());

    return std::format("{} {}", state.scaled_value, kSuffixes[state.suffix_index]);
};

/**
 * @brief Formats a generic count with SI suffixes (e.g., "5.8 M").
 * @details Avoids floating-point power approximations (std::pow) by using a compile-time
 *          powers table and binary search to ensure precise boundary transitions.
 */
inline constexpr auto format_count = [](std::uint64_t count) -> std::string {
    static constexpr std::array kSuffixes   = { "", "K", "M", "G", "T" };
    static constexpr auto kPowersOfThousand = []() {
        std::array<std::uint64_t, kSuffixes.size()> powers {};
        std::ranges::generate(powers, [current = 1ULL]() mutable { return std::exchange(current, current * 1'000); });
        return powers;
    }();

    const auto upper_bound_it      = std::ranges::upper_bound(kPowersOfThousand, count);
    const std::size_t dist         = toSize(std::ranges::distance(kPowersOfThousand.begin(), upper_bound_it));
    const std::size_t suffix_index = safe_sub(dist, 1uz).value_or(0uz);
    const double scaled_value      = toDouble(count) / toDouble(kPowersOfThousand[suffix_index]);

    const auto state = format_impl::adjust_overflow(
        format_impl::format_state { scaled_value, suffix_index }, 1000.0, kSuffixes.size());

    return std::format("{}{}", state.scaled_value, kSuffixes[state.suffix_index]);
};

inline constexpr auto check_disk_space
    = [](const std::filesystem::path& path,
          std::uint64_t required_bytes) noexcept -> std::expected<void, std::error_code> {
    std::error_code ec;
    const auto absolute_path = fs::absolute(path, ec);
    if (ec) { return std::unexpected(ec); }

    const auto target_res = [absolute_path](this auto self, fs::path p) -> std::expected<fs::path, std::error_code> {
        std::error_code ec;
        if (fs::exists(p, ec)) { return p; }
        if (ec) { return std::unexpected(ec); }
        if (!p.has_parent_path() || p.parent_path() == p) { return p; }
        return self(p.parent_path());
    }(absolute_path);

    if (!target_res) { return std::unexpected(target_res.error()); }
    const auto target = *target_res;

    const auto space_info = fs::space(target, ec);
    if (ec) { return std::unexpected(ec); }

    const auto total_req = safe_add(required_bytes, config::kMinBufferBytes);
    if (!total_req) { return std::unexpected(std::make_error_code(std::errc::value_too_large)); }

    if (space_info.available < *total_req) {
        return std::unexpected(std::make_error_code(std::errc::no_space_on_device));
    }
    return {};
};

inline constexpr auto get_test_filename
    = []() noexcept -> std::string { return std::format("{}.{}", ::config::kTestFilename, posix::getpid()); };

inline constexpr auto cleanup_artifacts = []() noexcept {
    const std::string disk_file = get_test_filename();
    std::error_code error_status;
    if (fs::exists(disk_file, error_status)) { fs::remove(disk_file, error_status); }
};

inline constexpr auto capitalize = [](std::string_view text) noexcept -> std::string {
    if (text.empty()) { return {}; }

    if (string_utils::equals_ic<"zram">(text)) { return "ZRAM"; }

    if (std::isupper(toUChar(text.front()))) { return std::string(text); }

    std::string result_string(text);
    result_string.front() = toChar(std::toupper(toUChar(result_string.front())));
    return result_string;
};

template <cast::numeric_type T>
[[nodiscard]] std::expected<T, std::errc> parse_number(std::string_view input_view) noexcept {
    T value {};
    const auto [end_pointer, error_status]
        = std::from_chars(input_view.data(), input_view.data() + input_view.size(), value);
    if (error_status == std::errc()) {
        if (end_pointer == input_view.data() + input_view.size()) { return value; }
        return std::unexpected(std::errc::invalid_argument);
    }
    return std::unexpected(error_status);
}

[[nodiscard]] inline std::string format_sys_error(std::error_code ec, std::string_view operation) {
    return std::format("{} ({})", operation, ec.message());
}

[[nodiscard]] inline std::string format_sys_error(std::errc ec, std::string_view operation) {
    return format_sys_error(std::make_error_code(ec), operation);
}

[[nodiscard]] inline std::string format_sys_error(std::integral auto error_status, std::string_view operation) {
    return format_sys_error(posix::make_error(error_status), operation);
}

inline constexpr auto read_file = [](const std::filesystem::path& path) -> std::expected<std::string, std::error_code> {
    return posix::file::read_all(path);
};

template <typename T>
concept writeable_data = std::ranges::contiguous_range<T> && sizeof(std::ranges::range_value_t<T>) == 1;

[[nodiscard]] constexpr auto to_writeable_bytes(const auto& data) noexcept {
    const auto bytes { std::as_bytes(std::span { std::ranges::data(data), std::ranges::size(data) }) };
    using RawT = std::remove_cvref_t<decltype(data)>;
    if constexpr (!std::is_bounded_array_v<RawT>) { return bytes; }
    if (bytes.empty() || bytes.back() != std::byte { 0 }) { return bytes; }
    const auto new_size { safe_sub(bytes.size(), 1uz).value_or(0uz) };
    return bytes.first(new_size);
}

template <writeable_data T>
inline std::expected<std::size_t, posix::file_descriptor::write_failure> write_bytes(
    posix::file_descriptor::native_handle_type fd, T&& data) {
    return posix::file_descriptor::write_exact_raw(fd, to_writeable_bytes(data));
}

template <writeable_data T>
inline std::expected<std::size_t, posix::file_descriptor::write_failure> write_bytes(
    const posix::file_descriptor& fd, T&& data) {
    return write_bytes(fd.native_handle(), std::forward<T>(data));
}

template <writeable_data T>
inline std::expected<std::size_t, posix::file_descriptor::write_failure> write_bytes(const posix::file& f, T&& data) {
    return write_bytes(f.descriptor(), std::forward<T>(data));
}

inline constexpr auto write_file
    = [](const std::filesystem::path& path, std::string_view content) -> std::expected<void, std::error_code> {
    return posix::file::write_to(path, content);
};

/**
 * @brief Read a file and apply a parser, returning a fallback on failure.
 * @param path   Path to read.
 * @param parser Callable that takes std::string_view and returns T.
 * @param fallback Value returned if the file cannot be read.
 */
template <typename T, std::invocable<std::string_view> Parser>
    requires std::convertible_to<std::invoke_result_t<Parser, std::string_view>, T>
inline T parse_file_or(const std::filesystem::path& path, Parser&& parser, T fallback) {
    return read_file(path).transform(std::forward<Parser>(parser)).value_or(std::move(fallback));
}

/**
 * @brief Range adapter that splits by a delimiter and transforms each part into a string_view.
 * @param delim The character delimiter.
 * @warning The resulting string_views are non-owning.
 * The source string must outlive the pipeline.
 */
inline auto split_to_sv(char delim) {
    return std::views::split(delim) | std::views::transform([](auto&& range) { return std::string_view(range); });
}

/**
 * @brief Range adapter that tokenizes by a single delimiter character, yielding non-empty string_view parts.
 * @param delim The character delimiter.
 * @warning The resulting string_views are non-owning.
 * The source string must outlive the pipeline.
 */
inline auto tokenize_sv(char delim) {
    return std::views::split(delim) | std::views::transform([](auto&& part) { return std::string_view(part); })
        | std::views::filter([](std::string_view token) { return !token.empty(); });
}

/**
 * @brief Range adapter that tokenizes by any leading/trailing whitespace (space, tab), yielding non-empty string_view
 * parts.
 * @note Resulting range is input_range only (not forward_range) due to
 * join_view caching of prvalue inner ranges (C++23 P2328R1).
 * Safe for single-pass use only.
 * @warning The resulting string_views are non-owning.
 * The source string must outlive the pipeline.
 */
inline auto tokenize_sv() {
    return std::views::split(' ') | std::views::transform([](auto&& range) { return std::string_view(range); })
        | std::views::filter([](std::string_view token) { return !token.empty(); })
        | std::views::transform([](std::string_view token) {
              /** @note secondary split by tab inline, avoid nested lazy view dangling */
              return token | std::views::split('\t')
                  | std::views::transform([](auto&& sub_range) { return std::string_view(sub_range); })
                  | std::views::filter([](std::string_view sub_token) { return !sub_token.empty(); });
          })
        | std::views::join;
}

template <std::ranges::input_range KeysRange>
    requires std::convertible_to<std::ranges::range_reference_t<KeysRange>, std::string_view>
inline std::optional<std::string_view> lookup_info_field(std::string_view content, const KeysRange& keys) {
    auto lines = std::views::split(content, '\n') | std::views::transform([](auto raw) {
        return std::string_view(raw);
    }) | std::views::transform([](std::string_view line) {
        const auto colon = line.find(':');
        return (colon == std::string_view::npos)
            ? std::pair { std::string_view {}, std::string_view {} }
            : std::pair { trim_sv(line.substr(0, colon)), trim_sv(line.substr(colon + 1)) };
    }) | std::views::filter([&keys](const auto& pair) {
        return !pair.first.empty() && !pair.second.empty() && std::ranges::any_of(keys, [&pair](std::string_view key) {
            return string_utils::equals_ic(pair.first, key);
        });
    }) | std::views::transform([](const auto& pair) { return pair.second; })
        | std::views::take(1);

    auto it = lines.begin();
    return (it != lines.end()) ? std::optional<std::string_view> { *it } : std::nullopt;
}

/**
 * @brief Generates a unique 64-bit seed or transaction identifier.
 */
[[nodiscard]] inline std::uint64_t generate_seed() noexcept {
    static const std::uint64_t kProcessSalt { []() noexcept {
        const auto [lo, hi] { posix::get_system_entropy() };
        return lo ^ hi;
    }() };
    static std::atomic<std::uint64_t> sequence { 0 };

    const auto seq { sequence.fetch_add(1, std::memory_order_relaxed) };
    const auto raw_tsc { stx::tsc::rdtsc() };
    const auto tsc { (raw_tsc ^ std::rotr(raw_tsc, 31u)) * 0xbf58476d1ce4e5b9ULL };
    const auto seed { (kProcessSalt ^ tsc) + seq };

    return prng::SplitMix64 { seed }();
}

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
#include "scope.hpp"
#include "tsc.hpp"

#include <algorithm>
#include <array>
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

/** @brief Compile-time tag for selecting the arithmetic operation in @ref safe_arith. */
enum class overflow_op {
    add,
    mul,
    sub
};

namespace overflow_impl {

/** @brief Primary template — intentionally undefined; forces an explicit specialization per operation. */
template <overflow_op Op> struct builtin_overflow;

/** @brief Dispatches to @c __builtin_add_overflow. */
template <> struct builtin_overflow<overflow_op::add> {
    template <cast::standard_integer_type T> [[nodiscard]] static constexpr bool apply(T lhs, T rhs, T* out) noexcept {
        return __builtin_add_overflow(lhs, rhs, out);
    }
};

/** @brief Dispatches to @c __builtin_mul_overflow. */
template <> struct builtin_overflow<overflow_op::mul> {
    template <cast::standard_integer_type T> [[nodiscard]] static constexpr bool apply(T lhs, T rhs, T* out) noexcept {
        return __builtin_mul_overflow(lhs, rhs, out);
    }
};

/** @brief Dispatches to @c __builtin_sub_overflow. */
template <> struct builtin_overflow<overflow_op::sub> {
    template <cast::standard_integer_type T> [[nodiscard]] static constexpr bool apply(T lhs, T rhs, T* out) noexcept {
        return __builtin_sub_overflow(lhs, rhs, out);
    }
};

} // namespace overflow_impl

/**
 * @brief Overflow-checked arithmetic on unsigned integers.
 *
 * The operation is selected at compile time via @ref overflow_impl::builtin_overflow specialization.
 * No runtime branching occurs for the operation itself — only the mandatory overflow sentinel check remains.
 *
 * @tparam Op  The arithmetic operation to perform (@ref overflow_op).
 * @tparam T   Any standard integer type (signed or unsigned, per @ref cast::standard_integer_type).
 * @return The result, or @c std::nullopt on overflow.
 */
template <overflow_op Op, cast::standard_integer_type T>
[[nodiscard]] constexpr auto safe_arith(T lhs, T rhs) noexcept -> std::optional<T> {
    T result {};
    return overflow_impl::builtin_overflow<Op>::apply(lhs, rhs, &result) ? std::nullopt : std::optional<T> { result };
}

/** @brief Safely multiply two @c uint64_t values with overflow checking. */
inline constexpr auto safe_mul
    = [](std::uint64_t lhs, std::uint64_t rhs) noexcept { return safe_arith<overflow_op::mul>(lhs, rhs); };

/** @brief Safely add two @c uint64_t values with overflow checking. */
inline constexpr auto safe_add
    = [](std::uint64_t lhs, std::uint64_t rhs) noexcept { return safe_arith<overflow_op::add>(lhs, rhs); };

/** @brief Safely subtract two @c uint64_t values with overflow checking. */
inline constexpr auto safe_sub
    = [](std::uint64_t lhs, std::uint64_t rhs) noexcept { return safe_arith<overflow_op::sub>(lhs, rhs); };

/**
 * @brief Checks if a value equals any of the specified template constants.
 *
 * Dispatched at compile time using fold expressions.
 */
template <auto... Vals, typename T> [[nodiscard]] constexpr bool is_one_of(const T& val) noexcept {
    return ((val == Vals) || ...);
}

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
    const std::size_t width = get_term_width();
    std::println("{:-<{}}", "", width);
};

inline constexpr auto print_centered_header = [](std::string_view text) noexcept {
    const std::size_t width    = get_term_width();
    const std::size_t text_len = text.length();

    if (text_len >= width - 2) {
        std::println("{}", text);
        return;
    }

    const std::size_t remaining = width - text_len - 2;
    const std::size_t left_pad  = remaining / 2;
    const std::size_t right_pad = remaining - left_pad;

    std::println("{0:-<{1}} {2} {0:-<{3}}", "", left_pad, text, right_pad);
};

inline constexpr auto trim_sv = [](std::string_view str) noexcept -> std::string_view {
    const auto first = str.find_first_not_of(" \t\n\r\v\f");
    if (first == std::string_view::npos) { return {}; }
    const auto last = str.find_last_not_of(" \t\n\r\v\f");
    return str.substr(first, last - first + 1);
};

inline constexpr auto trim = [](const std::string& str) -> std::string { return std::string(trim_sv(str)); };

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
    double scaled_value;
    std::size_t decimal_places;
    std::size_t suffix_index;
};

inline constexpr auto adjust_overflow = [](format_state& state, double base, std::size_t max_suffixes) noexcept {
    constexpr std::array kFactors = { 1.0, 10.0 };
    const double factor           = kFactors[state.decimal_places];
    const double rounded          = std::round(state.scaled_value * factor) / factor;

    const bool overflow = (rounded >= base) && (toSize(safe_add(state.suffix_index, 1uz).value_or(0uz)) < max_suffixes);
    state.suffix_index  = toSize(safe_add(state.suffix_index, toSize(+overflow)).value_or(state.suffix_index));

    const std::array kScaleValues = { state.scaled_value, rounded / base };
    state.scaled_value            = kScaleValues[toSize(+overflow)];
    state.decimal_places          = std::min<std::size_t>(toSize(std::llround(state.scaled_value * 10.0) % 10), 1uz);
};

} // namespace format_impl

/**
 * @brief Formats a byte count into a human-readable string (e.g., "1.2 MB").
 * @details Evaluates the scale index and dynamic precision in a single allocation pass
 *          without string mutations or float subtraction instability.
 */
inline constexpr auto format_bytes = [](std::uint64_t bytes) -> std::string {
    static constexpr std::array kSuffixes = { "B", "KB", "MB", "GB", "TB" };

    const std::size_t bits     = toSize(std::bit_width(bytes));
    const std::size_t shift    = toSize(safe_sub(bits, toSize(+(bits > 0))).value_or(0uz));
    std::size_t suffix_index   = std::min<std::size_t>(shift / 10uz, kSuffixes.size() - 1uz);
    double scaled_value        = toDouble(bytes) / toDouble(1ULL << toSize(safe_mul(suffix_index, 10uz).value_or(0uz)));
    std::size_t decimal_places = std::min<std::size_t>(toSize(std::llround(scaled_value * 10.0) % 10), 1uz);

    format_impl::format_state state { scaled_value, decimal_places, suffix_index };
    format_impl::adjust_overflow(state, 1024.0, kSuffixes.size());

    return std::format("{:.{}f} {}", state.scaled_value, state.decimal_places, kSuffixes[state.suffix_index]);
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

    const auto upper_bound_it = std::upper_bound(kPowersOfThousand.begin(), kPowersOfThousand.end(), count);
    std::size_t suffix_index
        = toSize(safe_sub(toSize(std::distance(kPowersOfThousand.begin(), upper_bound_it)), 1uz).value_or(0uz));
    double scaled_value        = toDouble(count) / toDouble(kPowersOfThousand[suffix_index]);
    std::size_t decimal_places = std::min<std::size_t>(toSize(std::llround(scaled_value * 10.0) % 10), 1uz);

    format_impl::format_state state { scaled_value, decimal_places, suffix_index };
    format_impl::adjust_overflow(state, 1000.0, kSuffixes.size());

    return std::format("{:.{}f}{}", state.scaled_value, state.decimal_places, kSuffixes[state.suffix_index]);
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

    if (text == "zram" || text == "Zram") { return "ZRAM"; }

    if (std::isupper(toUChar(text[0]))) { return std::string(text); }

    std::string result_string(text);
    result_string[0] = toChar(std::toupper(toUChar(result_string[0])));
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

inline std::expected<std::size_t, posix::file_descriptor::write_failure> write_all(
    posix::file_descriptor::native_handle_type fd, std::span<const std::byte> data) {
    return posix::file_descriptor::write_exact_raw(fd, data);
}

inline std::expected<std::size_t, posix::file_descriptor::write_failure> write_all(
    posix::file_descriptor::native_handle_type fd, std::string_view data) {
    return write_all(fd, std::as_bytes(std::span { data.data(), data.size() }));
}

template <typename T>
concept writeable_data = std::convertible_to<T, std::span<const std::byte>> || std::convertible_to<T, std::string_view>;

template <writeable_data T>
inline std::expected<std::size_t, posix::file_descriptor::write_failure> write_all(
    const posix::file_descriptor& fd, T&& data) {
    return write_all(fd.native_handle(), std::forward<T>(data));
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
    const auto content = read_file(path);
    if (!content) { return fallback; }
    return std::forward<Parser>(parser)(*content);
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

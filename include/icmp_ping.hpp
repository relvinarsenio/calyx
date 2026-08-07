/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "file_descriptor.hpp"
#include "numeric_cast.hpp"
#include "posix.hpp"
#include "random_engine.hpp"
#include "tsc.hpp"
#include "utils.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <optional>
#include <poll.h>
#include <ranges>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <type_traits>

namespace stx::icmp {

/**
 * @brief Validates POSIX socket address structures.
 */
template <typename T>
concept socket_address = posix::socket_address<T>;

/**
 * @brief Enforces wire-format layout integrity, zero-padding, and field ordering for ICMP frames (RFC 792 / RFC 4443).
 */
template <typename T>
concept icmp_request_packet = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> && requires(T pkt) {
    { pkt.header };
    { pkt.payload };
    requires(
        std::is_same_v<decltype(pkt.header), struct icmphdr> || std::is_same_v<decltype(pkt.header), struct icmp6_hdr>);
    requires sizeof(T) == (sizeof(pkt.header) + sizeof(pkt.payload));
    requires offsetof(T, header) == 0uz;
    requires offsetof(T, payload) == sizeof(pkt.header);
};

static_assert(ICMP_ECHO == 8, "ICMP_ECHO macro must match RFC 792 type value (8)");
static_assert(ICMP_ECHOREPLY == 0, "ICMP_ECHOREPLY macro must match RFC 792 type value (0)");
static_assert(ICMP6_ECHO_REQUEST == 128, "ICMP6_ECHO_REQUEST macro must match RFC 4443 type value (128)");
static_assert(ICMP6_ECHO_REPLY == 129, "ICMP6_ECHO_REPLY macro must match RFC 4443 type value (129)");

/**
 * @brief Diagnostic ICMP Echo ping service using datagram and raw socket fallback (RFC 792, RFC 4443).
 */
class icmp_ping {
public:
    /**
     * @brief Pings IPv4 host availability via unprivileged ICMP Echo (RFC 792).
     *
     * @param ip IPv4 target address string.
     * @param timeout Ping deadline budget.
     * @return bool True if host replied with a valid ICMP Echo Reply; false otherwise.
     */
    [[nodiscard]] static bool ping_ipv4(std::string_view ip, std::chrono::milliseconds timeout) noexcept {
        return ping_host<struct sockaddr_in>(
            ip, timeout, AF_INET, IPPROTO_ICMP, build_echo_request_v4, receive_echo_reply_v4, is_valid_echo_reply_v4);
    }

    /**
     * @brief Pings IPv6 host availability via unprivileged ICMPv6 Echo (RFC 4443).
     *
     * @param ip IPv6 target address string.
     * @param timeout Ping deadline budget.
     * @return bool True if host replied with a valid ICMPv6 Echo Reply; false otherwise.
     */
    [[nodiscard]] static bool ping_ipv6(std::string_view ip, std::chrono::milliseconds timeout) noexcept {
        return ping_host<struct sockaddr_in6>(ip, timeout, AF_INET6, IPPROTO_ICMPV6, build_echo_request_v6,
            receive_echo_reply_v6, is_valid_echo_reply_v6);
    }

private:
    static constexpr std::uint64_t kGoldenRatio64 { 0x9e3779b97f4a7c15ULL };
    static constexpr std::uint32_t kMaxIpPacketSize { IP_MAXPACKET };
    static constexpr std::uint32_t kIpIhlMask { 0x0Fu };
    static constexpr std::uint8_t kIpVersionMask { 0xF0u };
    static constexpr std::uint8_t kIPv4HeaderVersion { 0x40u };

    /**
     * @brief 64-bit dynamic nonce payload for transaction correlation (RFC 791, RFC 1122 Sec 3.2.2.6).
     */
    struct alignas(std::uint64_t) echo_payload {
        std::array<std::byte, 8uz> data {};
    };

    struct alignas(std::uint64_t) echo_request_v4 {
        struct icmphdr header {};
        echo_payload payload {};
    };

    struct alignas(std::uint64_t) echo_request_v6 {
        struct icmp6_hdr header {};
        echo_payload payload {};
    };

    struct alignas(std::uint64_t) echo_reply_v4 {
        struct icmphdr header {};
        echo_payload payload {};
    };

    struct alignas(std::uint64_t) echo_reply_v6 {
        struct icmp6_hdr header {};
        echo_payload payload {};
    };

    /**
     * @brief Validates ICMP Echo Reply (type, code, sequence, 64-bit transaction nonce; omits ID for SOCK_DGRAM).
     */
    [[nodiscard]] static bool is_valid_echo_reply_v4(const echo_reply_v4& reply, const echo_request_v4& req) noexcept {
        return reply.header.type == ICMP_ECHOREPLY && reply.header.code == 0
            && reply.header.un.echo.sequence == req.header.un.echo.sequence && reply.payload.data == req.payload.data;
    }

    /**
     * @brief Validates ICMPv6 Echo Reply (type, code, sequence, 64-bit transaction nonce).
     */
    [[nodiscard]] static bool is_valid_echo_reply_v6(const echo_reply_v6& reply, const echo_request_v6& req) noexcept {
        return reply.header.icmp6_type == ICMP6_ECHO_REPLY && reply.header.icmp6_code == 0
            && reply.header.icmp6_seq == req.header.icmp6_seq && reply.payload.data == req.payload.data;
    }

    /**
     * @brief Generates unique 64-bit transaction nonce for echo request correlation and stale reply filtering.
     */
    [[nodiscard]] static std::uint64_t generate_nonce() noexcept {
        static thread_local const std::uint8_t tl_marker {};
        const auto thread_addr { std::bit_cast<std::uint64_t>(&tl_marker) };
        const auto tsc { stx::tsc::rdtsc() };
        const auto pid { toULong(posix::getpid()) };
        const auto seed { safe_mul(pid ^ tsc ^ thread_addr, kGoldenRatio64).value_or(tsc) };
        return prng::SplitMix64 { seed }();
    }

    /**
     * @brief Computes 16-bit Internet Checksum with ones' complement carry folding (RFC 1071 Sec 4.1).
     */
    [[nodiscard]] static std::uint16_t calculate_checksum(std::span<const std::byte> buffer) noexcept {
        const std::uint32_t raw_sum { std::ranges::fold_left(
            buffer | std::views::chunk(2uz), 0u, [](std::uint32_t acc, auto chunk) noexcept {
                std::array<std::byte, 2> word_bytes {};
                std::copy_n(chunk.data(), chunk.size(), word_bytes.data());
                return safe_add(acc, std::bit_cast<std::uint16_t>(word_bytes)).value_or(acc);
            }) };

        const auto fold_carry { [](std::uint32_t sum_val) noexcept -> std::uint16_t {
            const auto sum1 { safe_add(sum_val & 0xFFFFu, sum_val >> 16u).value_or(sum_val) };
            const auto sum2 { safe_add(sum1 & 0xFFFFu, sum1 >> 16u).value_or(sum1) };
            return toUShort((~sum2) & 0xFFFFu);
        } };

        return fold_carry(raw_sum);
    }

    [[nodiscard]] static echo_request_v4 build_echo_request_v4(pid_t pid) noexcept {
        const std::uint64_t nonce { generate_nonce() };
        const std::uint16_t ident { toUShort((toULong(pid) ^ (nonce & 0xFFFFu)) & 0xFFFFu) };
        const std::uint16_t seq { toUShort((nonce >> 16u) & 0xFFFFu) };

        echo_request_v4 request {};
        request.header.type             = ICMP_ECHO;
        request.header.code             = 0;
        request.header.un.echo.id       = htons(ident);
        request.header.un.echo.sequence = htons(seq);
        request.payload.data            = std::bit_cast<std::array<std::byte, 8uz>>(nonce);
        request.header.checksum         = calculate_checksum(std::as_bytes(std::span { &request, 1uz }));
        return request;
    }

    [[nodiscard]] static echo_request_v6 build_echo_request_v6(pid_t pid) noexcept {
        const std::uint64_t nonce { generate_nonce() };
        const std::uint16_t ident { toUShort((toULong(pid) ^ (nonce & 0xFFFFu)) & 0xFFFFu) };
        const std::uint16_t seq { toUShort((nonce >> 16u) & 0xFFFFu) };

        echo_request_v6 request {};
        request.header.icmp6_type = ICMP6_ECHO_REQUEST;
        request.header.icmp6_code = 0;
        request.header.icmp6_id   = htons(ident);
        request.header.icmp6_seq  = htons(seq);
        request.payload.data      = std::bit_cast<std::array<std::byte, 8uz>>(nonce);
        return request;
    }

    /**
     * @brief Polls file descriptor for I/O readiness up to deadline.
     */
    [[nodiscard]] static bool poll_fd(
        posix::file_descriptor::native_handle_type fd, short events, std::chrono::milliseconds timeout) noexcept {
        pollfd pfd { .fd = fd, .events = events, .revents = 0 };
        return posix::poll(std::span { &pfd, 1uz }, timeout)
            .transform([&pfd, events](std::int32_t count) noexcept { return count > 0 && (pfd.revents & events) != 0; })
            .value_or(false);
    }

    /**
     * @brief Computes IPv4 header length in bytes (`IHL * 4`) to locate ICMP header offset (RFC 791 Sec 3.1).
     */
    [[nodiscard]] static constexpr std::uint32_t calculate_icmp_offset(std::span<const std::byte> buf) noexcept {
        if (buf.size() < sizeof(struct iphdr) + sizeof(struct icmphdr)) { return 0u; }
        const std::uint32_t ihl { std::to_integer<std::uint32_t>(buf.front()) & kIpIhlMask };
        return safe_mul(ihl, 4u).value_or(0u);
    }

    /**
     * @brief Safely extracts IPv4 ICMP header at calculated byte offset (RFC 792).
     */
    [[nodiscard]] static auto extract_icmphdr(std::span<const std::byte> buf, std::uint32_t offset) noexcept
        -> std::expected<struct icmphdr, std::error_code> {
        if (safe_add(offset, sizeof(struct icmphdr)).value_or(kMaxIpPacketSize) > buf.size()) {
            return std::unexpected(std::make_error_code(std::errc::message_size));
        }
        std::array<std::byte, sizeof(struct icmphdr)> hdr_bytes {};
        std::ranges::copy(buf | std::views::drop(offset) | std::views::take(sizeof(struct icmphdr)), hdr_bytes.begin());
        return std::bit_cast<struct icmphdr>(hdr_bytes);
    }

    /**
     * @brief Safely extracts IPv6 ICMPv6 header (RFC 4443).
     */
    [[nodiscard]] static auto extract_icmp6_hdr(std::span<const std::byte> buf) noexcept
        -> std::expected<struct icmp6_hdr, std::error_code> {
        if (buf.size() < sizeof(struct icmp6_hdr)) {
            return std::unexpected(std::make_error_code(std::errc::message_size));
        }
        std::array<std::byte, sizeof(struct icmp6_hdr)> hdr_bytes {};
        std::ranges::copy(buf | std::views::take(sizeof(struct icmp6_hdr)), hdr_bytes.begin());
        return std::bit_cast<struct icmp6_hdr>(hdr_bytes);
    }

    /**
     * @brief Extracts the echo payload from a receive buffer at a given byte offset.
     */
    [[nodiscard]] static auto extract_echo_payload(std::span<const std::byte> buf, std::uint32_t offset) noexcept
        -> std::expected<echo_payload, std::error_code> {
        if (safe_add(offset, toUInt(sizeof(echo_payload))).value_or(kMaxIpPacketSize) > buf.size()) {
            return std::unexpected(std::make_error_code(std::errc::message_size));
        }
        std::array<std::byte, sizeof(echo_payload)> pl_bytes {};
        std::ranges::copy(buf | std::views::drop(offset) | std::views::take(sizeof(echo_payload)), pl_bytes.begin());
        return std::bit_cast<echo_payload>(pl_bytes);
    }

    /**
     * @brief Parses ICMPv4 header + nonce payload from a receive buffer.
     *
     * SOCK_DGRAM delivers the ICMP message without the IPv4 header (LWN.net/443051); SOCK_RAW includes the IPv4 header.
     */
    [[nodiscard]] static auto parse_reply_icmphdr_v4(std::span<const std::byte> buf) noexcept
        -> std::expected<echo_reply_v4, std::error_code> {
        if (buf.empty()) { return std::unexpected(std::make_error_code(std::errc::message_size)); }

        const auto first_byte { std::to_integer<std::uint8_t>(buf.front()) };
        const bool is_ip_hdr { (first_byte & kIpVersionMask) == kIPv4HeaderVersion };
        const std::uint32_t icmp_offset { is_ip_hdr ? calculate_icmp_offset(buf) : 0u };

        const auto hdr { extract_icmphdr(buf, icmp_offset) };
        if (!hdr) { return std::unexpected(hdr.error()); }

        const auto payload_offset { safe_add(icmp_offset, toUInt(sizeof(struct icmphdr))).value_or(kMaxIpPacketSize) };
        const auto pl { extract_echo_payload(buf, payload_offset) };
        if (!pl) { return std::unexpected(pl.error()); }

        return echo_reply_v4 { .header = *hdr, .payload = *pl };
    }

    /**
     * @brief Receives IPv4 ICMP reply header using minimum reassembly buffer size (RFC 791).
     */
    [[nodiscard]] static std::optional<echo_reply_v4> receive_echo_reply_v4(const posix::file_descriptor& fd) noexcept {
        std::array<std::byte, 576uz> recv_buf {};
        struct sockaddr_in reply_addr {};

        return posix::recvfrom(fd, std::span { recv_buf }, reply_addr)
            .and_then([&recv_buf](std::size_t bytes) noexcept {
                return parse_reply_icmphdr_v4(std::span { recv_buf }.first(bytes));
            })
            .transform([](echo_reply_v4 reply) noexcept -> std::optional<echo_reply_v4> { return reply; })
            .value_or(std::nullopt);
    }

    /**
     * @brief Parses ICMPv6 header + nonce payload from a receive buffer.
     *
     * Kernel always strips the IPv6 header for IPPROTO_ICMPV6 sockets; header is at offset 0.
     */
    [[nodiscard]] static auto parse_reply_icmp6hdr_v6(std::span<const std::byte> buf) noexcept
        -> std::expected<echo_reply_v6, std::error_code> {
        const auto hdr { extract_icmp6_hdr(buf) };
        if (!hdr) { return std::unexpected(hdr.error()); }

        const auto pl { extract_echo_payload(buf, toUInt(sizeof(struct icmp6_hdr))) };
        if (!pl) { return std::unexpected(pl.error()); }

        return echo_reply_v6 { .header = *hdr, .payload = *pl };
    }

    /**
     * @brief Receives IPv6 ICMPv6 reply header using minimum link MTU buffer size (RFC 8200 Sec 5).
     */
    [[nodiscard]] static std::optional<echo_reply_v6> receive_echo_reply_v6(const posix::file_descriptor& fd) noexcept {
        std::array<std::byte, 1280uz> recv_buf {};
        struct sockaddr_in6 reply_addr {};

        return posix::recvfrom(fd, std::span { recv_buf }, reply_addr)
            .and_then([&recv_buf](std::size_t bytes) noexcept {
                return parse_reply_icmp6hdr_v6(std::span { recv_buf }.first(bytes));
            })
            .transform([](echo_reply_v6 reply) noexcept -> std::optional<echo_reply_v6> { return reply; })
            .value_or(std::nullopt);
    }

    /**
     * @brief Computes remaining ping deadline budget to prevent timeout overruns.
     */
    [[nodiscard]] static inline auto get_remaining_timeout(std::chrono::steady_clock::time_point t0,
        std::chrono::milliseconds timeout) noexcept -> std::chrono::milliseconds {
        const auto elapsed { std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0) };
        if (elapsed >= timeout) { return std::chrono::milliseconds(0); }
        return std::chrono::milliseconds(safe_sub(timeout.count(), elapsed.count()).value_or(0));
    }

    /**
     * @brief Unified ICMP ping sequence template for IPv4 and IPv6 targets.
     */
    template <socket_address Addr, typename RequestBuilder, typename ReplyReceiver, typename ReplyCheck>
        requires std::invocable<RequestBuilder, pid_t>
        && icmp_request_packet<std::invoke_result_t<RequestBuilder, pid_t>>
    [[nodiscard]] static bool ping_host(std::string_view ip, std::chrono::milliseconds timeout, std::int32_t af,
        std::int32_t proto, RequestBuilder&& build_req, ReplyReceiver&& recv_reply,
        ReplyCheck&& is_valid_reply) noexcept {
        const auto dest_addr_opt { [ip, af]() -> std::optional<Addr> {
            Addr sa {};
            if constexpr (requires { sa.sin_family; }) {
                sa.sin_family = toUShort(af);
                sa.sin_port   = 0;
                if (!posix::inet_pton(af, ip, sa.sin_addr)) { return std::nullopt; }
            } else {
                sa.sin6_family = toUShort(af);
                sa.sin6_port   = 0;
                if (!posix::inet_pton(af, ip, sa.sin6_addr)) { return std::nullopt; }
            }
            return sa;
        }() };

        if (!dest_addr_opt) { return false; }
        const auto dest_addr { *dest_addr_opt };
        const auto t0 { std::chrono::steady_clock::now() };

        auto sock_res {
            posix::socket(af, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, proto).or_else([af, proto](auto) noexcept {
                return posix::socket(af, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, proto);
            })
        };

        if (!sock_res) { return false; }

        posix::file_descriptor fd { std::move(*sock_res) };
        const auto request { build_req(posix::getpid()) };

        if (!posix::sendto(fd, std::as_bytes(std::span { &request, 1uz }), dest_addr)) { return false; }

        const auto rem_timeout { get_remaining_timeout(t0, timeout) };
        if (rem_timeout <= std::chrono::milliseconds(0)) { return false; }

        if (!poll_fd(fd.native_handle(), POLLIN, rem_timeout)) { return false; }

        const auto reply { recv_reply(fd) };
        return reply.has_value() && is_valid_reply(*reply, request);
    }
};

[[nodiscard]] inline bool ping_ipv4(std::string_view ip, std::chrono::milliseconds timeout) noexcept {
    return icmp_ping::ping_ipv4(ip, timeout);
}

[[nodiscard]] inline bool ping_ipv6(std::string_view ip, std::chrono::milliseconds timeout) noexcept {
    return icmp_ping::ping_ipv6(ip, timeout);
}

} // namespace stx::icmp

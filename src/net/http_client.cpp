/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/**
 * @file http_client.cpp
 * @brief Pragmatic HTTP client implementation using libcurl.
 *
 * @note Error Handling Strategy:
 * This project uses a hybrid approach:
 * 1. `std::expected` for high-level operations where callers benefit from rich error context.
 * 2. `noexcept` + `std::string` return (or similar) for low-level system info collection
 *    where "Unknown" or empty results are acceptable fallbacks.
 */

#include "http_client.hpp"

#include "config.hpp"
#include "file_descriptor.hpp"
#include "http_context.hpp"
#include "interrupts.hpp"
#include "random_engine.hpp"
#include "scope.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <curl/curl.h>
#include <expected>
#include <filesystem>
#include <format>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using prng::Xoshiro256PlusPlus;

std::expected<std::string, std::string> extract_origin(std::string_view url) noexcept {
    curl::unique_url_handle handle { curl_url() };
    if (!handle) { return std::unexpected("Failed to create CURLU handle"); }

    if (curl_url_set(
            handle.get(), CURLUPART_URL, std::string { url }.c_str(), CURLU_GUESS_SCHEME | CURLU_DEFAULT_SCHEME)
        != CURLUE_OK) {
        return std::unexpected(std::format("Invalid URL for origin extraction: {}", url));
    }

    const auto get_part = [&handle](CURLUPart part, unsigned int flags = 0) -> std::optional<std::string> {
        char* buf = nullptr;
        if (curl_url_get(handle.get(), part, &buf, flags) == CURLUE_OK && buf) {
            curl::unique_char_ptr ptr { buf };
            return std::string { buf };
        }
        return std::nullopt;
    };

    const auto scheme = get_part(CURLUPART_SCHEME).transform([](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    });

    const auto host
        = get_part(CURLUPART_HOST, CURLU_PUNYCODE).or_else([&get_part] { return get_part(CURLUPART_HOST); });
    const auto port = get_part(CURLUPART_PORT, CURLU_NO_DEFAULT_PORT);

    if (!scheme || !host) { return std::unexpected("URL missing scheme or host"); }

    if (*scheme != "http" && *scheme != "https") {
        return std::unexpected(std::format("Unsupported scheme: {}", *scheme));
    }

    const std::string formatted_host = [host] {
        if (host->contains(':') && !host->starts_with('[')) { return std::format("[{}]", *host); }
        return *host;
    }();

    if (port) { return std::format("{}://{}:{}", *scheme, formatted_host, *port); }
    return std::format("{}://{}", *scheme, formatted_host);
}

constexpr char kUserAgent[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                              "Chrome/144.0.0.0 Safari/537.36 Edg/144.0.0.0";

struct StringWriteContext {
    std::reference_wrapper<std::string> output;
};

struct FileWriteContext {
    posix::file fd;
    std::size_t bytes_written { 0 };
    std::error_code last_error {};
};

template <typename T>
concept object_type = std::is_object_v<T>;

template <object_type T> [[nodiscard]] T& user_data_as(void* userdata) noexcept {
    return *static_cast<T*>(userdata);
}

std::size_t write_string_callback(void* ptr, std::size_t size, std::size_t nmemb, void* userdata) noexcept {
    if (!ptr || !userdata) [[unlikely]] { return 0; }

    const auto total_opt = safe_mul(size, nmemb);
    if (!total_opt) [[unlikely]] { return 0; }

    auto& ctx = user_data_as<StringWriteContext>(userdata);
    auto& out = ctx.output.get();

    const auto space_left = safe_sub(out.max_size(), out.size());
    if (!space_left || *total_opt > *space_left) { return 0; }

    try {
        out.append(static_cast<const char*>(ptr), toSize(*total_opt));
        return *total_opt;
    } catch (...) { return 0; }
}

std::size_t write_file_callback(void* ptr, std::size_t size, std::size_t nmemb, void* userdata) noexcept {
    if (!ptr || !userdata) [[unlikely]] { return 0; }

    auto total_opt = safe_mul(size, nmemb);
    if (!total_opt) { return 0; }

    auto& ctx = user_data_as<FileWriteContext>(userdata);

    if (!ctx.fd) [[unlikely]] { return 0; }

    auto bytes = std::span { static_cast<const std::byte*>(ptr), *total_opt };
    auto res   = ctx.fd.write_exact(bytes);
    if (!res) {
        ctx.bytes_written += res.error().bytes_transferred;
        ctx.last_error = res.error().error;
        return res.error().bytes_transferred;
    }

    ctx.bytes_written += *total_opt;
    return *total_opt;
}

} // namespace

std::expected<void, std::error_code> CurlHeaders::add(const std::string& header) noexcept {
    auto* appended = curl_slist_append(list_.get(), header.c_str());
    if (!appended) { return std::unexpected(std::make_error_code(std::errc::not_enough_memory)); }
    if (!list_) { list_.reset(appended); }
    return {};
}

std::expected<void, std::string> HttpClient::setup_browser_impersonation(
    CurlHeaders& headers, std::string_view url) noexcept {
    return extract_origin(url).and_then([this, &headers](
                                            const std::string& origin) -> std::expected<void, std::string> {
        const std::string referer_url = std::format("{}/", origin);

        using namespace std::string_view_literals;
        static constexpr auto kBrowserHeaders = std::array {
            "sec-ch-ua: \"Not A;Brand\";v=\"99\", \"Chromium\";v=\"144\", \"Microsoft Edge\";v=\"144\""sv,
            "sec-ch-ua-mobile: ?0"sv, "sec-ch-ua-platform: \"Windows\""sv, "dnt: 1"sv, "sec-gpc: 1"sv,
            "upgrade-insecure-requests: 1"sv, "cache-control: no-cache"sv, "pragma: no-cache"sv,
            "sec-fetch-site: same-origin"sv, "sec-fetch-mode: cors"sv, "sec-fetch-dest: empty"sv, "priority: u=4"sv,
            "accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,application/json,text/plain,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7"sv,
            "accept-language: id,en;q=0.9,en-US;q=0.8,en-GB;q=0.7"sv
        };

        return this->set_option(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1)
            .and_then([&headers, &origin, &referer_url] {
                return headers.add(std::format("origin: {}", origin))
                    .and_then([&headers, &referer_url] { return headers.add(std::format("referer: {}", referer_url)); })
                    .transform_error(
                        [](auto ec) { return format_sys_error(ec.value(), "Failed to add dynamic headers"); });
            })
            .and_then([&headers] {
                return std::ranges::fold_left(kBrowserHeaders, std::expected<void, std::string> {},
                    [&headers](std::expected<void, std::string> acc, std::string_view hdr) {
                        return acc.and_then([&headers, hdr] {
                            return headers.add(std::string { hdr }).transform_error([](std::error_code ec) {
                                return format_sys_error(ec.value(), "Failed to add browser header");
                            });
                        });
                    });
            })
            .and_then([this, &referer_url] { return this->set_option(CURLOPT_REFERER, referer_url.c_str()); })
            .and_then([this] { return this->set_option(CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br"); })
            .and_then([this] {
                return this->set_option(CURLOPT_TLS13_CIPHERS,
                    "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");
            })
            .and_then([this] {
                return this->set_option(CURLOPT_SSL_CIPHER_LIST,
                    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
                    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
                    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305");
            });
    });
}

int sockopt_callback([[maybe_unused]] void* clientp, curl_socket_t curlfd, [[maybe_unused]] curlsocktype purpose) {
    constexpr std::int32_t kTtl = config::kTcpTtl;

    const auto ok_v4 = posix::file_descriptor::setsockopt_raw(curlfd, IPPROTO_IP, IP_TTL, kTtl);
    const auto ok_v6 = posix::file_descriptor::setsockopt_raw(curlfd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, kTtl);

    if (!ok_v4 && !ok_v6) { return CURL_SOCKOPT_ERROR; }

    return CURL_SOCKOPT_OK;
}

std::expected<posix::file, std::string> open_download_directory(const std::filesystem::path& filepath) {
    const auto dir_path = filepath.has_parent_path() ? filepath.parent_path() : std::filesystem::path { "." };

    return posix::file::open(dir_path, O_RDONLY | O_DIRECTORY).transform_error([&filepath](std::error_code ec) {
        return format_sys_error(ec.value(), std::format("Cannot access directory for '{}'", filepath.string()));
    });
}

std::expected<std::pair<std::filesystem::path, posix::file>, std::string> open_temp_download_file(
    const std::filesystem::path& filepath) {
    thread_local Xoshiro256PlusPlus engine { std::random_device {}() };
    for (auto attempt : std::views::iota(0u, 256u)) {
        const std::uint64_t nonce = engine();
        auto temp_path            = filepath;
        temp_path += std::format(".tmp.{}.{}.{}", posix::getpid(), nonce, attempt);

        auto opened = posix::file::open(
            temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (opened) { return std::pair { std::move(temp_path), std::move(*opened) }; }

        if (opened.error() != std::errc::file_exists) {
            return std::unexpected(
                format_sys_error(opened.error().value(), std::format("Cannot save file '{}'", filepath.string())));
        }
    }

    return std::unexpected(
        std::format("Cannot create temporary file for '{}' (too many collisions)", filepath.string()));
}

std::expected<void, std::string> sync_file_or_error(
    posix::file& file, const std::filesystem::path& filepath, std::string_view context) {
    return file.sync().transform_error([&filepath, context](std::error_code ec) {
        return format_sys_error(ec.value(), std::format("Failed to sync {} '{}'", context, filepath.string()));
    });
}

std::expected<void, std::string> finalize_download_file(
    const std::filesystem::path& temp_path, const std::filesystem::path& filepath) {
    std::error_code ec;
    std::filesystem::rename(temp_path, filepath, ec);

    return (!ec) ? std::expected<void, std::string> {}
                 : std::unexpected(format_sys_error(
                       ec.value(), std::format("Failed to finalize download '{}'", filepath.string())));
}

std::string map_download_request_error(
    const FileWriteContext& write_ctx, const std::filesystem::path& filepath, std::string network_error) {
    if (!write_ctx.last_error) { return network_error; }

    const auto write_err = format_sys_error(
        write_ctx.last_error.value(), std::format("Download failed while writing '{}'", filepath.string()));

    return (write_ctx.bytes_written == 0)
        ? write_err
        : std::format("{} ({} transferred before failure)", write_err, format_bytes(toULong(write_ctx.bytes_written)));
}

struct HttpClient::GetState final : public HttpClient::RequestState {
    std::string response;
    StringWriteContext write_ctx { std::ref(response) };
    CurlHeaders headers;
    std::string url_str;
    explicit GetState(std::string_view url)
        : url_str(url) {}

    [[nodiscard]] std::expected<void, std::string> finalize(CURLcode code) noexcept override {
        result_status = (code != CURLE_OK)
            ? std::unexpected(std::format("Network error ({})", curl_easy_strerror(code)))
            : std::expected<void, std::string> {};
        return *result_status;
    }

    [[nodiscard]] std::expected<std::string, std::string> result_string() const override {
        if (!result_status) { return std::unexpected("Request not finalized"); }
        return result_status->transform([this] { return response; });
    }
};

struct HttpClient::ConnectivityState final : public HttpClient::RequestState {
    std::string host;
    explicit ConnectivityState(std::string_view h)
        : host(h) {}

    [[nodiscard]] std::expected<void, std::string> finalize(CURLcode code) noexcept override {
        result_status = (code != CURLE_OK)
            ? std::unexpected(std::format("Connectivity check failed: ({})", curl_easy_strerror(code)))
            : std::expected<void, std::string> {};
        return *result_status;
    }

    [[nodiscard]] std::expected<void, std::string> result_void() const override {
        return result_status.value_or(std::unexpected("Request not finalized"));
    }
};

static std::expected<void, std::string> ensure_not_pending(const std::shared_ptr<HttpClient::RequestState>& state) {
    if (state && !state->result_status.has_value()) {
        return std::unexpected(
            "Cannot re-prepare HttpClient while its current request is still in progress (pending finalization)");
    }
    return {};
}

namespace curl_helpers {
[[nodiscard]] inline std::expected<curl::shared_handle, std::string> curl_easy_init_safe() noexcept {
    return posix::expect_result<posix::error_style::pointer>(curl_easy_init())
        .transform([](CURL* p) { return curl::shared_handle(p, curl_easy_cleanup); })
        .transform_error([](auto) { return std::string("Failed to create curl handle"); });
}

[[nodiscard]] inline std::expected<void, CURLMcode> expect_multi(CURLMcode code) noexcept {
    if (code == CURLM_OK) { return {}; }
    return std::unexpected(code);
}

[[nodiscard]] inline std::expected<curl::unique_multi_handle, std::string> curl_multi_init_safe() noexcept {
    return posix::expect_result<posix::error_style::pointer>(curl_multi_init())
        .transform([](CURLM* p) { return curl::unique_multi_handle(p); })
        .transform_error([](auto) { return std::string("Failed to create curl multi handle"); });
}
} // namespace curl_helpers

HttpClient::HttpClient(std::shared_ptr<void> token, curl::shared_handle handle) noexcept
    : token_(std::move(token))
    , handle_(std::move(handle)) {}

std::expected<HttpClient, std::string> HttpClient::create() {
    return HttpContext::ensure_initialized().and_then([](auto token) {
        return curl_helpers::curl_easy_init_safe().transform(
            [token = std::move(token)](
                curl::shared_handle handle) mutable { return HttpClient(std::move(token), std::move(handle)); });
    });
}

std::expected<void, std::string> HttpClient::apply_base_options() {
    const auto cert           = curl::get_embedded_cert();
    const void* cert_data_ptr = static_cast<const void*>(cert.data());
    struct curl_blob blob {};
    blob.data  = const_cast<void*>(cert_data_ptr);
    blob.len   = cert.size();
    blob.flags = CURL_BLOB_NOCOPY;

    return set_option(CURLOPT_SOCKOPTFUNCTION, sockopt_callback)
        .and_then([this] { return set_option(CURLOPT_TCP_KEEPALIVE, 1L); })
        .and_then([this] { return set_option(CURLOPT_NOSIGNAL, 1L); })
        .and_then([this] { return set_option(CURLOPT_USERAGENT, kUserAgent); })
        .and_then([this] { return set_option(CURLOPT_FOLLOWLOCATION, 1L); })
        .and_then([this, &blob] { return set_option(CURLOPT_CAINFO_BLOB, &blob); })
        .and_then([this] { return set_option(CURLOPT_SSL_VERIFYPEER, 1L); })
        .and_then([this] { return set_option(CURLOPT_SSL_VERIFYHOST, 2L); })
        .and_then([this] {
            return set_option(
                CURLOPT_XFERINFOFUNCTION, +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                    return check_interrupted() ? 1 : 0;
                });
        })
        .and_then([this] { return set_option(CURLOPT_NOPROGRESS, 0L); });
}

std::expected<void, std::string> HttpClient::perform_request() {
    return (check_interrupted() ? std::unexpected(std::string { config::kInterruptMsg })
                                : std::expected<void, std::string> {})
        .and_then([this]() -> std::expected<void, std::string> {
            const auto res = curl_easy_perform(handle_.get());
            if (res == CURLE_OK) { return {}; }
            return std::unexpected(res == CURLE_ABORTED_BY_CALLBACK
                    ? std::string { config::kInterruptMsg }
                    : std::format("Network error ({})", curl_easy_strerror(res)));
        });
}

std::expected<void, std::string> HttpClient::download(std::string_view url, const std::filesystem::path& filepath) {
    curl_easy_reset(handle_.get());
    return apply_base_options()
        .and_then([&filepath] { return open_download_directory(filepath); })
        .and_then([this, url, &filepath](posix::file dir_file) {
            return open_temp_download_file(filepath).and_then([this, url, &filepath, dir_file = std::move(dir_file)](
                                                                  auto temp_state) mutable {
                auto [temp_path, temp_file] = std::move(temp_state);
                FileWriteContext write_ctx { .fd = std::move(temp_file) };
                CurlHeaders headers;

                scope_exit remover { [&temp_path]() noexcept {
                    std::error_code ec;
                    std::filesystem::remove(temp_path, ec);
                } };

                return setup_browser_impersonation(headers, url)
                    .transform_error([](auto err) { return std::format("Failed to impersonate browser: {}", err); })
                    .and_then([this, &headers, &write_ctx, url_str = std::string { url }] {
                        return set_option(CURLOPT_URL, url_str.c_str())
                            .and_then([this, &headers] { return set_option(CURLOPT_HTTPHEADER, headers.get()); })
                            .and_then([this] { return set_option(CURLOPT_WRITEFUNCTION, write_file_callback); })
                            .and_then([this, &write_ctx] { return set_option(CURLOPT_WRITEDATA, &write_ctx); })
                            .and_then(
                                [this] { return set_option(CURLOPT_TIMEOUT, config::kSpeedtestDlTimeout.count()); })
                            .and_then([this] {
                                return set_option(CURLOPT_CONNECTTIMEOUT, config::kHttpConnectTimeout.count());
                            })
                            .and_then([this] { return perform_request(); });
                    })
                    .transform_error([&write_ctx, &filepath](std::string err) {
                        return map_download_request_error(write_ctx, filepath, std::move(err));
                    })
                    .and_then([&write_ctx, &filepath] { return sync_file_or_error(write_ctx.fd, filepath, "file"); })
                    .and_then(
                        [&dir_file, &filepath] { return sync_file_or_error(dir_file, filepath, "directory for"); })
                    .and_then([&temp_path, &filepath] { return finalize_download_file(temp_path, filepath); })
                    .and_then(
                        [&dir_file, &filepath] { return sync_file_or_error(dir_file, filepath, "directory for"); })
                    .transform([&remover] { remover.release(); });
            });
        });
}

std::expected<void, std::string> HttpClient::check_connectivity(std::string_view host) {
    curl_easy_reset(handle_.get());
    const std::string url = std::format("http://{}", host);

    return set_option(CURLOPT_NOSIGNAL, 1L)
        .and_then([this] { return set_option(CURLOPT_SOCKOPTFUNCTION, sockopt_callback); })
        .and_then([this] {
            return set_option(
                CURLOPT_XFERINFOFUNCTION, +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                    return check_interrupted() ? 1 : 0;
                });
        })
        .and_then([this] { return set_option(CURLOPT_NOPROGRESS, 0L); })
        .and_then([this, &url] { return set_option(CURLOPT_URL, url.c_str()); })
        .and_then([this] { return set_option(CURLOPT_CONNECT_ONLY, 1L); })
        .and_then([this] { return set_option(CURLOPT_TIMEOUT, config::kCheckConnTimeout.count()); })
        .and_then([this] { return set_option(CURLOPT_CONNECTTIMEOUT, config::kCheckConnConnectTimeout.count()); })
        .and_then([this] { return set_option(CURLOPT_FORBID_REUSE, 1L); })
        .and_then([this] { return perform_request(); })
        .transform_error([](std::string err) { return std::format("Connectivity check failed: {}", err); });
}

std::expected<void, std::string> HttpClient::prepare_get(std::string_view url) {
    return ensure_not_pending(current_state_).and_then([this, url_view = url]() -> std::expected<void, std::string> {
        curl_easy_reset(handle_.get());
        current_state_.reset();

        return apply_base_options().and_then([this, url_view]() {
            auto state = std::make_shared<GetState>(url_view);
            return setup_browser_impersonation(state->headers, url_view)
                .transform_error([](auto err) { return std::format("Failed to impersonate browser: {}", err); })
                .and_then([this, state]() mutable -> std::expected<void, std::string> {
                    return set_option(CURLOPT_URL, state->url_str.c_str())
                        .and_then([this] { return set_option(CURLOPT_WRITEFUNCTION, write_string_callback); })
                        .and_then([this, state] { return set_option(CURLOPT_WRITEDATA, &state->write_ctx); })
                        .and_then([this, state] { return set_option(CURLOPT_HTTPHEADER, state->headers.get()); })
                        .and_then([this] { return set_option(CURLOPT_TIMEOUT, config::kHttpTimeout.count()); })
                        .and_then(
                            [this] { return set_option(CURLOPT_CONNECTTIMEOUT, config::kHttpConnectTimeout.count()); })
                        .and_then([this, state] { return set_option(CURLOPT_PRIVATE, state.get()); })
                        .transform([this, state = std::move(state)]() mutable { current_state_ = std::move(state); });
                });
        });
    });
}

std::expected<void, std::string> HttpClient::prepare_connectivity_check(std::string_view host) {
    return ensure_not_pending(current_state_).and_then([this, host_view = host]() -> std::expected<void, std::string> {
        curl_easy_reset(handle_.get());
        current_state_.reset();

        return apply_base_options().and_then([this, host_str = std::string { host_view }]() mutable {
            auto state            = std::make_shared<ConnectivityState>(host_str);
            const std::string url = std::format("http://{}", host_str);

            return set_option(CURLOPT_URL, url.c_str())
                .and_then([this] { return set_option(CURLOPT_CONNECT_ONLY, 1L); })
                .and_then([this] { return set_option(CURLOPT_TIMEOUT, config::kCheckConnTimeout.count()); })
                .and_then(
                    [this] { return set_option(CURLOPT_CONNECTTIMEOUT, config::kCheckConnConnectTimeout.count()); })
                .and_then([this] { return set_option(CURLOPT_FORBID_REUSE, 1L); })
                .and_then([this, state] { return set_option(CURLOPT_PRIVATE, state.get()); })
                .transform([this, state = std::move(state)] { current_state_ = std::move(state); });
        });
    });
}

std::expected<std::string, std::string> HttpClient::get_result_string() const {
    if (!current_state_) { return std::unexpected("No active request"); }
    return current_state_->result_string();
}

std::expected<void, std::string> HttpClient::get_result_void() const {
    if (!current_state_) { return std::unexpected("No active request"); }
    return current_state_->result_void();
}

MultiHttpClient::MultiHttpClient(std::shared_ptr<void> token, curl::unique_multi_handle handle) noexcept
    : token_(std::move(token))
    , handle_(std::move(handle)) {}

std::expected<MultiHttpClient, std::string> MultiHttpClient::create() {
    return HttpContext::ensure_initialized().and_then([](auto token) {
        return curl_helpers::curl_multi_init_safe().transform(
            [token = std::move(token)](curl::unique_multi_handle handle) mutable {
                return MultiHttpClient(std::move(token), std::move(handle));
            });
    });
}

std::expected<void, std::string> MultiHttpClient::add_handle(const HttpClient& client) {
    if (!client.current_state_) {
        return std::unexpected("Cannot add HttpClient to MultiHttpClient: no request prepared");
    }

    if (client.current_state_->result_status.has_value()) {
        return std::unexpected("Cannot add HttpClient to MultiHttpClient: prepared request is already finalized");
    }

    auto* easy = client.handle_.get();
    if (std::ranges::any_of(attached_requests_, [easy](const auto& pair) { return pair.first.get() == easy; })) {
        return std::unexpected("HttpClient handle is already attached to this MultiHttpClient");
    }

    const auto res = curl_multi_add_handle(handle_.get(), easy);
    if (res == CURLM_OK) {
        attached_requests_.emplace_back(client.handle_, client.current_state_);
        return {};
    }
    return std::unexpected(std::format("Failed to add easy handle: {}", curl_multi_strerror(res)));
}

std::expected<void, std::string> MultiHttpClient::perform() {
    int running = 0;

    const auto finalize_msg = [this](const CURLMsg* msg) noexcept {
        if (msg->msg != CURLMSG_DONE) { return; }
        const auto easy = msg->easy_handle;

        if (auto it = std::ranges::find_if(attached_requests_, [easy](const auto& p) { return p.first.get() == easy; });
            it != attached_requests_.end()) {
            it->second->finalize_if_pending(msg->data.result);
            curl_multi_remove_handle(handle_.get(), easy);
            attached_requests_.erase(it);
            return;
        }

        void* state_ptr = nullptr;
        if (curl_easy_getinfo(easy, CURLINFO_PRIVATE, &state_ptr) == CURLE_OK && state_ptr) {
            static_cast<HttpClient::RequestState*>(state_ptr)->finalize_if_pending(msg->data.result);
        }

        curl_multi_remove_handle(handle_.get(), easy);
    };

    scope_exit cleanup_guard { [this, &finalize_msg]() {
        int left = 0;
        while (const auto* msg = curl_multi_info_read(handle_.get(), &left)) {
            finalize_msg(msg);
        }

        for (const auto& [handle, state] : attached_requests_) {
            curl_multi_remove_handle(handle_.get(), handle.get());
            state->finalize_if_pending(CURLE_ABORTED_BY_CALLBACK);
        }
        attached_requests_.clear();
    } };

    auto run_step = [this, &running]() -> std::expected<void, std::string> {
        return curl_helpers::expect_multi(curl_multi_perform(handle_.get(), &running))
            .transform_error(
                [](CURLMcode code) { return std::format("curl_multi_perform failed: {}", curl_multi_strerror(code)); })
            .and_then([this, &running]() -> std::expected<void, std::string> {
                if (running > 0) {
                    return curl_helpers::expect_multi(curl_multi_poll(handle_.get(), nullptr, 0, 100, nullptr))
                        .transform_error([](CURLMcode code) {
                            return std::format("curl_multi_poll failed: {}", curl_multi_strerror(code));
                        });
                }
                return {};
            });
    };

    do {
        if (auto res = run_step(); !res) { return res; }
        if (check_interrupted()) { return std::unexpected(std::string { config::kInterruptMsg }); }
    } while (running > 0);

    return {};
}

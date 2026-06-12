/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <concepts>
#include <curl/curl.h>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace posix {
class file_descriptor;
}

namespace curl {
using shared_handle       = std::shared_ptr<CURL>;
using unique_multi_handle = std::unique_ptr<CURLM, decltype([](CURLM* p) {
    if (p) { curl_multi_cleanup(p); }
})>;
using unique_url_handle   = std::unique_ptr<CURLU, decltype([](CURLU* p) {
    if (p) { curl_url_cleanup(p); }
})>;
using unique_char_ptr     = std::unique_ptr<char, decltype([](char* p) {
    if (p) { curl_free(p); }
})>;
using unique_slist_ptr    = std::unique_ptr<struct curl_slist, decltype([](auto* p) {
    if (p) { curl_slist_free_all(p); }
})>;

template <typename T>
concept scalar = std::is_scalar_v<T>;

} // namespace curl

class CurlHeaders {
    curl::unique_slist_ptr list_;

public:
    CurlHeaders() = default;

    CurlHeaders(const CurlHeaders&)            = delete;
    CurlHeaders& operator=(const CurlHeaders&) = delete;
    CurlHeaders(CurlHeaders&&)                 = default;
    CurlHeaders& operator=(CurlHeaders&&)      = default;

    [[nodiscard]] std::expected<void, std::error_code> add(const std::string& header) noexcept;
    [[nodiscard]] curl_slist* get() const noexcept { return list_.get(); }
};

class HttpClient {
public:
    [[nodiscard]] static std::expected<HttpClient, std::string> create();
    ~HttpClient()                            = default;
    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&)                 = default;
    HttpClient& operator=(HttpClient&&)      = default;

    [[nodiscard]] std::expected<void, std::string> download(
        std::string_view url, const std::filesystem::path& filepath);
    [[nodiscard]] std::expected<void, std::string> check_connectivity(std::string_view host);

    [[nodiscard]] std::expected<void, std::string> prepare_get(std::string_view url);
    [[nodiscard]] std::expected<void, std::string> prepare_connectivity_check(std::string_view host);

    [[nodiscard]] std::expected<void, std::string> get_result_void() const;
    [[nodiscard]] std::expected<std::string, std::string> get_result_string() const;

    struct RequestState {
        virtual ~RequestState() = default;

        RequestState()                               = default;
        RequestState(const RequestState&)            = delete;
        RequestState& operator=(const RequestState&) = delete;
        RequestState(RequestState&&)                 = delete;
        RequestState& operator=(RequestState&&)      = delete;

        std::optional<std::expected<void, std::string>> result_status;

        [[nodiscard]] virtual std::expected<void, std::string> finalize(CURLcode code) noexcept = 0;

        void finalize_if_pending(CURLcode code) noexcept {
            if (!result_status.has_value()) { [[maybe_unused]] auto _ = finalize(code); }
        }

        [[nodiscard]] virtual std::expected<std::string, std::string> result_string() const {
            return std::unexpected("Not a string result request");
        }
        [[nodiscard]] virtual std::expected<void, std::string> result_void() const {
            return std::unexpected("Not a connectivity check request");
        }
    };

    friend class MultiHttpClient;

private:
    explicit HttpClient(std::shared_ptr<void> token, curl::shared_handle handle) noexcept;

    template <curl::scalar T>
    [[nodiscard]] std::expected<void, std::string> set_option(CURLoption option, T value) noexcept {
        const auto res = curl_easy_setopt(handle_.get(), option, value);
        return res == CURLE_OK ? std::expected<void, std::string> {}
                               : std::unexpected(std::format("curl_easy_setopt failed: {}", curl_easy_strerror(res)));
    }

    [[nodiscard]] std::expected<void, std::string> setup_browser_impersonation(
        CurlHeaders& headers, std::string_view url) noexcept;

    std::shared_ptr<void> token_;
    curl::shared_handle handle_;
    std::shared_ptr<RequestState> current_state_;

    struct GetState;
    struct ConnectivityState;

    [[nodiscard]] std::expected<void, std::string> apply_base_options();
    [[nodiscard]] std::expected<void, std::string> perform_request();
};

class MultiHttpClient {
public:
    [[nodiscard]] static std::expected<MultiHttpClient, std::string> create();

    MultiHttpClient(const MultiHttpClient&)            = delete;
    MultiHttpClient& operator=(const MultiHttpClient&) = delete;
    MultiHttpClient(MultiHttpClient&&)                 = default;
    MultiHttpClient& operator=(MultiHttpClient&&)      = default;

    [[nodiscard]] std::expected<void, std::string> add_handle(const HttpClient& client);
    /**
     * @brief Execute all attached requests concurrently.
     *
     * @note MultiHttpClient shares ownership of the attached easy handles and request states
     * for the duration of perform() and cleanup, ensuring safety even if the originating
     * HttpClient is destroyed.
     */
    [[nodiscard]] std::expected<void, std::string> perform();

private:
    MultiHttpClient(std::shared_ptr<void> token, curl::unique_multi_handle handle) noexcept;

    std::shared_ptr<void> token_;
    curl::unique_multi_handle handle_;
    std::vector<std::pair<curl::shared_handle, std::shared_ptr<HttpClient::RequestState>>> attached_requests_;
};
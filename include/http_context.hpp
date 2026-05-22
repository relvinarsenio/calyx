/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <curl/curl.h>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <string>

namespace curl_global_state {
struct GlobalLibraryState {
    ~GlobalLibraryState() { curl_global_cleanup(); }
};

inline std::weak_ptr<GlobalLibraryState> global_state_weak;
inline std::mutex global_state_mtx;
} // namespace curl_global_state

class HttpContext {
public:
    [[nodiscard]] static std::expected<HttpContext, std::string> create() {
        return ensure_initialized().transform(
            [](std::shared_ptr<void> token) { return HttpContext(std::move(token)); });
    }

    ~HttpContext() = default;

    HttpContext(const HttpContext&)            = delete;
    HttpContext& operator=(const HttpContext&) = delete;
    HttpContext(HttpContext&&)                 = delete;
    HttpContext& operator=(HttpContext&&)      = delete;

    [[nodiscard]] static std::expected<std::shared_ptr<void>, std::string> ensure_initialized() {
        std::lock_guard lock(curl_global_state::global_state_mtx);

        if (auto state = curl_global_state::global_state_weak.lock(); state) { return state; }

        if (auto res = curl_global_init(CURL_GLOBAL_ALL); res != CURLE_OK) {
            return std::unexpected(std::format("Libcurl Initialization ({})", curl_easy_strerror(res)));
        }

        std::shared_ptr<curl_global_state::GlobalLibraryState> state;
        try {
            state = std::make_shared<curl_global_state::GlobalLibraryState>();
        } catch (...) {
            curl_global_cleanup();
            return std::unexpected("Failed to allocate global HTTP context state");
        }

        curl_global_state::global_state_weak = state;
        return state;
    }

private:
    explicit HttpContext(std::shared_ptr<void> token) noexcept
        : token_(std::move(token)) {}
    std::shared_ptr<void> token_;
};
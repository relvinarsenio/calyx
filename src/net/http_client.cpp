/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "include/http_client.hpp"
#include "include/config.hpp"
#include "include/embedded_cert.hpp"
#include "include/file_descriptor.hpp"
#include "include/interrupts.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <curl/curl.h>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <span>
#include <unistd.h>
#include <new>

namespace {

struct CurlSlistDeleter {
    void operator()(struct curl_slist* list) const noexcept {
        if (list)
            curl_slist_free_all(list);
    }
};

class CurlHeaders {
    std::unique_ptr<struct curl_slist, CurlSlistDeleter> list_;

   public:
    void add(const std::string& header) {
        auto new_head = curl_slist_append(list_.get(), header.c_str());
        if (!new_head) {
            throw std::bad_alloc();
        }

        list_.release();
        list_.reset(new_head);
    }

    struct curl_slist* get() const {
        return list_.get();
    }
};

void setup_browser_impersonation(CURL* handle, CurlHeaders& headers) {
    headers.add(
        "Accept: "
        "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/"
        "apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7");
    headers.add("Accept-Language: en-US,en;q=0.9");
    headers.add("Cache-Control: max-age=0");
    headers.add("Connection: keep-alive");
    headers.add(
        "Sec-Ch-Ua: \"Not_A Brand\";v=\"8\", \"Chromium\";v=\"120\", \"Google Chrome\";v=\"120\"");
    headers.add("Sec-Ch-Ua-Mobile: ?0");
    headers.add("Sec-Ch-Ua-Platform: \"Windows\"");
    headers.add("Sec-Fetch-Dest: document");
    headers.add("Sec-Fetch-Mode: navigate");
    headers.add("Sec-Fetch-Site: none");
    headers.add("Sec-Fetch-User: ?1");
    headers.add("Upgrade-Insecure-Requests: 1");

    curl_easy_setopt(handle, CURLOPT_USERAGENT, Config::HTTP_USER_AGENT.data());
    curl_easy_setopt(handle, CURLOPT_REFERER, "https://www.google.com/");
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br");

    struct curl_blob blob {};
    blob.data = const_cast<void*>(static_cast<const void*>(cacert_pem));
    blob.len = cacert_pem_len;
    curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &blob);
}

}  // namespace

HttpClient::HttpClient() : handle_(curl_easy_init(), curl_easy_cleanup) {
    if (!handle_)
        throw std::runtime_error("Failed to create curl handle");
}

size_t HttpClient::write_string(void* ptr,
                                size_t size,
                                size_t nmemb,
                                std::string* str_buffer) noexcept {
    try {
        size_t total_size = size * nmemb;
        std::span<const char> data_view(static_cast<const char*>(ptr), total_size);

        str_buffer->append(data_view.begin(), data_view.end());

        return total_size;
    } catch (...) {
        return 0;
    }
}

size_t HttpClient::write_file(void* ptr, size_t size, size_t nmemb, FileDescriptor* fd) noexcept {
    const size_t total_size = size * nmemb;
    const char* data_ptr = static_cast<const char*>(ptr);
    size_t remaining = total_size;

    while (remaining > 0) {
        ssize_t written = ::write(fd->get(), data_ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }

        if (written == 0) {
            return 0;
        }

        data_ptr += written;
        remaining -= static_cast<size_t>(written);
    }

    return total_size;
}

std::expected<std::string, std::string> HttpClient::get(const std::string& url) {
    curl_easy_reset(handle_.get());

    std::string response;
    CurlHeaders headers;
    setup_browser_impersonation(handle_.get(), headers);

    curl_easy_setopt(handle_.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_.get(), CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(handle_.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle_.get(), CURLOPT_HTTPHEADER, headers.get());

    curl_easy_setopt(handle_.get(), CURLOPT_TIMEOUT, Config::HTTP_TIMEOUT_SEC);
    curl_easy_setopt(handle_.get(), CURLOPT_CONNECTTIMEOUT, Config::HTTP_CONNECT_TIMEOUT_SEC);
    curl_easy_setopt(handle_.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle_.get(), CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle_.get(), CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(
        handle_.get(),
        CURLOPT_XFERINFOFUNCTION,
        +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return g_interrupted ? 1 : 0;
        });
    curl_easy_setopt(handle_.get(), CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(handle_.get());
    check_interrupted();

    if (res != CURLE_OK) {
        return std::unexpected(std::format("Network error: {}", curl_easy_strerror(res)));
    }
    return response;
}

std::expected<void, std::string> HttpClient::download(const std::string& url,
                                                      const std::string& filepath) {
    curl_easy_reset(handle_.get());

    int raw_fd = ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (raw_fd < 0) {
        return std::unexpected(
            std::format("Cannot save file '{}': {}", filepath, std::strerror(errno)));
    }
    FileDescriptor fd(raw_fd);

    CurlHeaders headers;
    setup_browser_impersonation(handle_.get(), headers);

    curl_easy_setopt(handle_.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle_.get(), CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(handle_.get(), CURLOPT_WRITEDATA, &fd);

    curl_easy_setopt(handle_.get(), CURLOPT_TIMEOUT, Config::SPEEDTEST_DL_TIMEOUT_SEC);
    curl_easy_setopt(handle_.get(), CURLOPT_CONNECTTIMEOUT, Config::HTTP_CONNECT_TIMEOUT_SEC);
    curl_easy_setopt(handle_.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle_.get(), CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(
        handle_.get(),
        CURLOPT_XFERINFOFUNCTION,
        +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return g_interrupted ? 1 : 0;
        });
    curl_easy_setopt(handle_.get(), CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(handle_.get());
    check_interrupted();

    if (res != CURLE_OK) {
        std::filesystem::remove(filepath);
        return std::unexpected(std::format("Download failed: {}", curl_easy_strerror(res)));
    }

    if (::fsync(fd.get()) == -1) {
        int saved_errno = errno;
        std::filesystem::remove(filepath);

        return std::unexpected(std::format("Failed to sync file '{}': {} (Code: {})",
                                           filepath,
                                           std::system_category().message(saved_errno),
                                           saved_errno));
    }

    return {};
}

bool HttpClient::check_connectivity(const std::string& host) {
    try {
        curl_easy_reset(handle_.get());
        curl_easy_setopt(handle_.get(), CURLOPT_URL, ("http://" + host).c_str());
        curl_easy_setopt(handle_.get(), CURLOPT_NOBODY, 1L);
        curl_easy_setopt(handle_.get(), CURLOPT_TIMEOUT, Config::CHECK_CONN_TIMEOUT_SEC);
        curl_easy_setopt(
            handle_.get(), CURLOPT_CONNECTTIMEOUT, Config::CHECK_CONN_CONNECT_TIMEOUT_SEC);
        curl_easy_setopt(handle_.get(), CURLOPT_USERAGENT, Config::HTTP_USER_AGENT.data());
        curl_easy_setopt(handle_.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle_.get(), CURLOPT_FORBID_REUSE, 1L);
        return curl_easy_perform(handle_.get()) == CURLE_OK;
    } catch (...) {
        return false;
    }
}
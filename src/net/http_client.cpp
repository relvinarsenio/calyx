#include "include/http_client.hpp"
#include "include/interrupts.hpp"
#include "include/config.hpp"
#include "include/embedded_cert.hpp"

#include <cerrno>
#include <filesystem>
#include <format>
#include <fstream>
#include <curl/curl.h>
#include <span>
#include <array>

namespace {

constexpr auto kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

struct CurlSlistDeleter {
    void operator()(struct curl_slist* list) const noexcept {
        if (list) curl_slist_free_all(list);
    }
};

class CurlHeaders {
    std::unique_ptr<struct curl_slist, CurlSlistDeleter> list_;

public:
    void add(const std::string& header) {
        auto new_head = curl_slist_append(list_.get(), header.c_str());
        if (new_head && !list_) {
            list_.reset(new_head);
        }
    }

    struct curl_slist* get() const { return list_.get(); }
};

void setup_browser_impersonation(CURL* handle, CurlHeaders& headers) {
    headers.add("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7");
    headers.add("Accept-Language: en-US,en;q=0.9");
    headers.add("Cache-Control: max-age=0");
    headers.add("Connection: keep-alive");
    headers.add("Sec-Ch-Ua: \"Not_A Brand\";v=\"8\", \"Chromium\";v=\"120\", \"Google Chrome\";v=\"120\"");
    headers.add("Sec-Ch-Ua-Mobile: ?0");
    headers.add("Sec-Ch-Ua-Platform: \"Windows\"");
    headers.add("Sec-Fetch-Dest: document");
    headers.add("Sec-Fetch-Mode: navigate");
    headers.add("Sec-Fetch-Site: none");
    headers.add("Sec-Fetch-User: ?1");
    headers.add("Upgrade-Insecure-Requests: 1");

    curl_easy_setopt(handle, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(handle, CURLOPT_REFERER, "https://www.google.com/");
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br");

    struct curl_blob blob{};
    blob.data = (void*)cacert_pem;
    blob.len = cacert_pem_len;
    blob.flags = CURL_BLOB_COPY;
    curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &blob);
}

}

HttpClient::HttpClient() : handle_(curl_easy_init(), curl_easy_cleanup) {
    if (!handle_) throw std::runtime_error("Failed to create curl handle");
}

size_t HttpClient::write_string(void* ptr, size_t size, size_t nmemb, std::string* s) noexcept {
    try {
        size_t total_size = size * nmemb;
        std::span<const char> data_view(static_cast<const char*>(ptr), total_size);
        
        s->append(data_view.begin(), data_view.end());
        
        return total_size;
    } catch (...) {
        return 0;
    }
}

size_t HttpClient::write_file(void* ptr, size_t size, size_t nmemb, std::ofstream* f) noexcept {
    try {
        size_t total_size = size * nmemb;
        std::span<const char> data_view(static_cast<const char*>(ptr), total_size);
        
        f->write(data_view.data(), static_cast<std::streamsize>(data_view.size()));
        
        if (!*f) return 0;
        return total_size;
    } catch (...) {
        return 0;
    }
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

    curl_easy_setopt(handle_.get(), CURLOPT_XFERINFOFUNCTION,
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

std::expected<void, std::string> HttpClient::download(const std::string& url, const std::string& filepath) {
    curl_easy_reset(handle_.get());

    std::ofstream outfile(filepath, std::ios::binary);
    if (!outfile) {
        return std::unexpected(std::format("Cannot save file '{}': {}", 
            filepath, std::system_category().message(errno)));
    }

    CurlHeaders headers;
    setup_browser_impersonation(handle_.get(), headers);

    curl_easy_setopt(handle_.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle_.get(), CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(handle_.get(), CURLOPT_WRITEDATA, &outfile);
    
    curl_easy_setopt(handle_.get(), CURLOPT_TIMEOUT, Config::SPEEDTEST_DL_TIMEOUT_SEC);
    curl_easy_setopt(handle_.get(), CURLOPT_CONNECTTIMEOUT, Config::HTTP_CONNECT_TIMEOUT_SEC);
    curl_easy_setopt(handle_.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle_.get(), CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(handle_.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return g_interrupted ? 1 : 0;
        });
    curl_easy_setopt(handle_.get(), CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(handle_.get());
    check_interrupted();

    if (res != CURLE_OK) {
        outfile.close();
        std::filesystem::remove(filepath);
        return std::unexpected(std::format("Download failed: {}", curl_easy_strerror(res)));
    }
    
    return {}; 
}

bool HttpClient::check_connectivity(const std::string& host) {
    try {
        curl_easy_reset(handle_.get());
        curl_easy_setopt(handle_.get(), CURLOPT_URL, ("http://" + host).c_str());
        curl_easy_setopt(handle_.get(), CURLOPT_NOBODY, 1L);
        curl_easy_setopt(handle_.get(), CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(handle_.get(), CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(handle_.get(), CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(handle_.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle_.get(), CURLOPT_FORBID_REUSE, 1L);
        return curl_easy_perform(handle_.get()) == CURLE_OK;
    } catch (...) {
        return false;
    }
}
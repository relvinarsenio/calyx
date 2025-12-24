#pragma once

#include <string>
#include <vector>
#include <memory>
#include <expected>

typedef void CURL;

class HttpClient {
public:
    HttpClient();
    ~HttpClient() = default;

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    std::expected<std::string, std::string> get(const std::string& url);
    std::expected<void, std::string> download(const std::string& url, const std::string& filepath);
    bool check_connectivity(const std::string& host);

private:
    std::unique_ptr<CURL, void(*)(CURL*)> handle_;

    static size_t write_string(void* ptr, size_t size, size_t nmemb, std::string* s) noexcept;
    static size_t write_file(void* ptr, size_t size, size_t nmemb, std::ofstream* f) noexcept;
};
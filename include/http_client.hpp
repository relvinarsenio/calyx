/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using CURL = void;

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
    std::unique_ptr<CURL, void (*)(CURL*)> handle_;

    static size_t write_string(void* ptr, size_t size, size_t nmemb, std::string* s) noexcept;
    static size_t write_file(void* ptr, size_t size, size_t nmemb, std::ofstream* file_stream) noexcept;
};
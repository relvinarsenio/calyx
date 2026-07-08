/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "http_client.hpp"
#include "posix_error.hpp"
#include "results.hpp"
#include "tgz_extractor.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <variant>

struct HttpError {
    std::string message {};
};

struct SpeedTestLogicError {
    std::string message {};
};

using SpeedTestError = std::variant<posix::SysCallError, archive::ExtractError, HttpError, SpeedTestLogicError>;

[[nodiscard]] std::string get_error_string(const SpeedTestError& err);

class SpeedTest {
    HttpClient& http_;

    std::filesystem::path base_dir_;
    std::filesystem::path cli_dir_;
    std::filesystem::path cli_path_;
    std::filesystem::path tgz_path_;

public:
    explicit SpeedTest(HttpClient& h, const std::filesystem::path& base_dir);
    SpeedTest(SpeedTest&&)            = default;
    SpeedTest& operator=(SpeedTest&&) = delete;

    SpeedTest(const SpeedTest&)            = delete;
    SpeedTest& operator=(const SpeedTest&) = delete;

    ~SpeedTest();

    [[nodiscard]] static std::expected<SpeedTest, SpeedTestError> create(HttpClient& h);

    [[nodiscard]] std::expected<void, SpeedTestError> install();
    [[nodiscard]] SpeedTestResult run();

    [[nodiscard]] std::filesystem::path get_base_dir() const { return base_dir_; }
};
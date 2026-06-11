/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "http_client.hpp"
#include "results.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <string_view>

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

    [[nodiscard]] static std::expected<SpeedTest, std::string> create(HttpClient& h);

    [[nodiscard]] std::expected<void, std::string> install();
    [[nodiscard]] SpeedTestResult run();

    [[nodiscard]] std::filesystem::path get_base_dir() const { return base_dir_; }
};
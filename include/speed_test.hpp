/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "http_client.hpp"
#include "results.hpp"

enum class SpinnerEvent { Start, Stop };
using SpinnerCallback = std::function<void(SpinnerEvent, std::string_view)>;

class SpeedTest {
    HttpClient& http_;

    std::filesystem::path base_dir_;
    std::filesystem::path cli_dir_;
    std::filesystem::path cli_path_;
    std::filesystem::path tgz_path_;

   public:
    explicit SpeedTest(HttpClient& h);

    ~SpeedTest();

    void install();
    SpeedTestResult run(const SpinnerCallback& spinner_cb = {});
};
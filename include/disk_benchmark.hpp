// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2025 Alfie Ardinata.

#pragma once

#include <cstddef>
#include <functional>
#include <stop_token>
#include <string_view>
#include <expected>
#include <string>

#include "results.hpp"

class DiskBenchmark {
public:
    static std::expected<DiskIORunResult, std::string> run_io_test(
        int size_mb,
        std::string_view label,
        const std::function<void(std::size_t, std::size_t, std::string_view)>& progress_cb = {},
        std::stop_token stop = {});
};
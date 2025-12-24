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
    static std::expected<DiskRunResult, std::string> run_write_test(
        int size_mb,
        std::string_view label,
        const std::function<void(std::size_t, std::size_t, std::string_view)>& progress_cb = {},
        std::stop_token stop = {});
};
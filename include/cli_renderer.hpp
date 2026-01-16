/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "results.hpp"
#include "speed_test.hpp"
#include <string>
#include <string_view>
#include <functional>

namespace CliRenderer {
void render_speed_results(const SpeedTestResult& result);
SpinnerCallback make_spinner_callback();

std::string create_progress_bar(int percent);
std::function<void(std::size_t, std::size_t, std::string_view)> make_progress_callback(
    int label_width);
void render_progress_line(std::string_view label, int percent, int label_width);
}  // namespace CliRenderer

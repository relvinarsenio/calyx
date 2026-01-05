// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2025 Alfie Ardinata.

#pragma once

#include "results.hpp"
#include "speed_test.hpp"

namespace CliRenderer {
    void render_speed_results(const SpeedTestResult& result);
    SpinnerCallback make_spinner_callback();
}

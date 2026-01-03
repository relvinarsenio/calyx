#pragma once

#include "results.hpp"
#include "speed_test.hpp"

namespace CliRenderer {
    void render_speed_results(const SpeedTestResult& result);
    SpinnerCallback make_spinner_callback();
}

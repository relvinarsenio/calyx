/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "latency_histogram.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct [[nodiscard]] DiskIOMetrics {
    metrics::LatencyHistogram histogram;
    double bw_bytes_per_sec = 0.0;
    double cv               = 0.0; ///< Coefficient of Variation (σ/μ) of latency — lower is more stable.
    double avg_latency_ms   = 0.0;
    double min_latency_ms   = 0.0;
    double max_latency_ms   = 0.0;
    double p50_latency_ms   = 0.0;
    double p95_latency_ms   = 0.0;
    double p99_latency_ms   = 0.0;
    double p999_latency_ms  = 0.0;
};

struct [[nodiscard]] DiskIORunResult {
    std::string label;
    DiskIOMetrics write;
    DiskIOMetrics read;
};

struct [[nodiscard]] SpeedEntryResult {
    std::string server_id;
    std::string node_name;
    double upload_mbps {};
    double download_mbps {};
    double latency_ms {};
    std::string loss;
    bool success = false;
    std::string error;
    bool rate_limited = false;
};

struct [[nodiscard]] SpeedTestResult {
    std::vector<SpeedEntryResult> entries;
    bool rate_limited = false;
};

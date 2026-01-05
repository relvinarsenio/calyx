// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2025 Alfie Ardinata.

#pragma once

#include <string>
#include <vector>

struct DiskIORunResult {
    std::string label;
    double write_mbps = 0.0;
    double read_mbps = 0.0;
};

struct DiskSuiteResult {
    std::vector<DiskIORunResult> runs;
    double average_write_mbps = 0.0;
    double average_read_mbps = 0.0;
};

struct SpeedEntryResult {
    std::string server_id;
    std::string node_name;
    double upload_mbps = 0.0;
    double download_mbps = 0.0;
    double latency_ms = 0.0;
    std::string loss;
    bool success = false;
    std::string error;    
    bool rate_limited = false;
};

struct SpeedTestResult {
    std::vector<SpeedEntryResult> entries;
    bool rate_limited = false;
};

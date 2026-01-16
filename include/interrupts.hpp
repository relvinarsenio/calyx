/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <atomic>
#include <csignal>
#include <stdexcept>

extern std::atomic<bool> g_interrupted;

void signal_handler(int) noexcept;
void check_interrupted();

class SignalGuard {
   public:
    SignalGuard();
    ~SignalGuard() = default;

    SignalGuard(const SignalGuard&) = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;
};
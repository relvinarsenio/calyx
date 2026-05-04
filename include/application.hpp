/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <expected>
#include <string>
#include <string_view>

/**
 * @brief The main entry point and coordinator for the Calyx application.
 *
 * Handles CLI argument parsing, execution flow, and scoped cleanup orchestration.
 */
class Application {
public:
    /**
     * @brief Executes the application logic based on provided arguments.
     * @return std::expected<void, std::string> containing success or error message.
     */
    [[nodiscard]] std::expected<void, std::string> run(int argc, char* argv[]);

private:
    /** @brief Displays command-line help information. */
    void show_help(std::string_view app_name) const;
    /** @brief Displays current application version. */
    void show_version() const;
};
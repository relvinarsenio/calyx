/*
 * Copyright (c) 2025-2026 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "application.hpp"
#include "cli_renderer.hpp"
#include "utils.hpp"

#include <clocale>
#include <expected>
#include <string>

int main(int argc, char* argv[]) {
    std::setlocale(LC_ALL, "");
    ui::TerminalGuard terminal_guard {};
    Application app {};
    return app.run(argc, argv)
        .transform([]() { return 0; })
        .or_else([](const std::string& err) -> std::expected<int, std::string> {
            print_error(err);
            return 1;
        })
        .value();
}

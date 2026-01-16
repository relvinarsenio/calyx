/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <string>

class Application {
   public:
    int run(int argc, char* argv[]);

   private:
    void show_help(const std::string& app_name) const;
    void show_version() const;
};
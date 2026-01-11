/*
 * Copyright (c) 2025 Alfie Ardinata
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "include/http_context.hpp"

#include <stdexcept>
#include <mutex>

#include <curl/curl.h>
#include <openssl/crypto.h>

namespace {
    static std::mutex init_mutex;
    static int reference_count = 0;
    static bool libraries_initialized = false;
}

HttpContext::HttpContext() {
    std::lock_guard<std::mutex> lock(init_mutex);
    
    if (reference_count == 0) {
        if (OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr) == 0) {
            throw std::runtime_error("Failed to initialize OpenSSL crypto library");
        }

        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            throw std::runtime_error("Failed to initialize libcurl globally");
        }
        
        libraries_initialized = true;
    }
    
    ++reference_count;
}

HttpContext::~HttpContext() {
    std::lock_guard<std::mutex> lock(init_mutex);
    
    --reference_count;
    
    if (reference_count == 0 && libraries_initialized) {
        // Last instance - cleanup global libraries
        curl_global_cleanup();
        libraries_initialized = false;
    }
}
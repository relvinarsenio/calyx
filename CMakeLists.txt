cmake_minimum_required(VERSION 3.24)

# Fix FetchContent timestamp warning (CMP0135), LTO Policy (CMP0069), & return() argument check (CMP0140)
cmake_policy(SET CMP0135 NEW)
cmake_policy(SET CMP0140 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

# =============================================================================
# Calyx - Rapid Linux Server Profiler
# =============================================================================
project(calyx VERSION 1.2.0 LANGUAGES CXX C)

# Ensure all libraries and targets are built with -fPIC (Required for static-pie)
# C++23 Required
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Global settings for security and optimization
set(CMAKE_TLS_VERIFY ON CACHE BOOL "Strict TLS verification for FetchContent" FORCE)

# Global Modern CMake settings (Forced for all dependencies)
set(CMAKE_C_VISIBILITY_PRESET hidden CACHE STRING "Hide C symbols by default" FORCE)
set(CMAKE_CXX_VISIBILITY_PRESET hidden CACHE STRING "Hide C++ symbols by default" FORCE)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON CACHE BOOL "Hide inline functions by default" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Enable PIC/PIE for static-pie binary" FORCE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON CACHE BOOL "Enable Link-Time Optimization (LTO)" FORCE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json for IDEs/clangd" FORCE)
set(CMAKE_LINK_SEARCH_START_STATIC ON CACHE BOOL "Prefer static libraries for find_library" FORCE)

# Force MinSizeRel build type globally for the project and all dependencies
set(CMAKE_BUILD_TYPE "MinSizeRel" CACHE STRING "Forced MinSizeRel" FORCE)

# =============================================================================
# Clean Logging Helpers (No Emojis)
# =============================================================================
function(log_section TITLE)
    message(STATUS "")
    message(STATUS "-- [ ${TITLE} ] -----------------------------------------------")
endfunction()

function(log_property KEY VALUE)
    string(SUBSTRING "${KEY}                            " 0 20 PADDED_KEY)
    message(STATUS "  - ${PADDED_KEY}: ${VALUE}")
endfunction()

# =============================================================================
# 1. Compiler Detection & CCACHE SETUP (CRITICAL: MUST BE TOP)
# =============================================================================
# Enable ccache if available (Global)
message(CHECK_START "Checking for ccache compiler launcher")
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(CHECK_PASS "found (${CCACHE_PROGRAM})")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
else()
    message(CHECK_FAIL "not found")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DetectCompiler)

# =============================================================================
# LLVM Full Stack Configuration
# =============================================================================
find_program(LLVM_AR llvm-ar REQUIRED)
find_program(LLVM_RANLIB llvm-ranlib REQUIRED)
find_program(LLVM_NM llvm-nm)
find_program(LLVM_STRIP llvm-strip)
find_program(LLVM_OBJCOPY llvm-objcopy REQUIRED)
find_program(LLD_LINKER ld.lld REQUIRED)

set(CMAKE_AR "${LLVM_AR}" CACHE FILEPATH "Archiver" FORCE)
set(CMAKE_RANLIB "${LLVM_RANLIB}" CACHE FILEPATH "Ranlib" FORCE)
set(CMAKE_LINKER "${LLD_LINKER}" CACHE FILEPATH "Linker" FORCE)
set(CMAKE_OBJCOPY "${LLVM_OBJCOPY}" CACHE FILEPATH "Objcopy" FORCE)

if(LLVM_NM)
    set(CMAKE_NM "${LLVM_NM}" CACHE FILEPATH "NM" FORCE)
endif()

if(LLVM_STRIP)
    set(CMAKE_STRIP "${LLVM_STRIP}" CACHE FILEPATH "Strip" FORCE)
endif()

set(CMAKE_LINKER_TYPE LLD CACHE STRING "Force LLD linker globally" FORCE)

log_section("LLVM Toolchain Stack")
log_property("AR" "${LLVM_AR}")
log_property("RANLIB" "${LLVM_RANLIB}")
log_property("LINKER" "${LLD_LINKER}")
log_property("OBJCOPY" "${LLVM_OBJCOPY}")
if(LLVM_STRIP)
    log_property("STRIP" "${LLVM_STRIP}")
endif()

# =============================================================================
# Security Hardening Flags (ALWAYS ON)
# =============================================================================
add_compile_options(
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=3
    -fstack-protector-strong
    -fstack-clash-protection
    -Wformat
    -Wformat-security
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    add_compile_options(-fcf-protection=full)
    add_link_options(-Wl,-z,shstk)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    add_compile_options(-mbranch-protection=standard)
endif()

add_link_options(
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
    -Wl,--exclude-libs,ALL
    -Wl,--build-id=sha1
)


# =============================================================================
# Optimization flags (Release)
# =============================================================================
if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" OR CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(
        -ffunction-sections
        -fdata-sections
    )
    add_link_options(
        -Wl,--gc-sections
        -Wl,--as-needed
    )
endif()

# =============================================================================
# 2. Configure Static Dependencies
# =============================================================================
include(StaticDeps)

# libcurl_static implicitly inherits LTO from global CMake settings
# =============================================================================
# 3. Compiler Configuration (Linking)
# =============================================================================
add_link_options(
    "-static"
    "-static-pie"
)

if(USE_LIBCXX)
    message(STATUS "Mode: Clang ${DETECTED_COMPILER_VERSION} + libc++")
    add_compile_options("-stdlib=libc++")
    add_link_options("-nostdlib++")
elseif(USE_LIBSTDCXX)
    message(STATUS "Mode: Clang ${DETECTED_COMPILER_VERSION} + libstdc++")
    add_link_options("-static-libstdc++")
endif()

# =============================================================================
# 4. Warning Flags & Architecture
# =============================================================================
add_compile_options(
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wsign-conversion
    -Wcast-qual -Wdouble-promotion -Wshadow -Wnull-dereference
    -Wunused-parameter -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align
    -Wfloat-equal -Wundef -Wimplicit-fallthrough -Wsuggest-override
    -Wpessimizing-move -Wredundant-move -Wvla -Wmisleading-indentation
    -Wparentheses -Waddress -Wswitch-enum -Wtautological-compare
    -ftrivial-auto-var-init=zero
)


if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    add_compile_options(-march=x86-64-v3)
endif()

# =============================================================================
# 5. Embedded Certificate Generation
# =============================================================================
set(CACERT_HASH "SHA256=86a1f3366afac7c6f8ae9f3c779ac221129328c43f0ab2b8817eb2f362a5025c")

message(CHECK_START "Downloading CA certificate bundle")

file(DOWNLOAD 
    "https://curl.se/ca/cacert-2026-05-14.pem" 
    "${CMAKE_BINARY_DIR}/cacert.pem"
    EXPECTED_HASH ${CACERT_HASH}
    TLS_VERIFY ON
    STATUS DOWNLOAD_STATUS
)

list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
list(GET DOWNLOAD_STATUS 1 STATUS_MESSAGE)

if(NOT STATUS_CODE EQUAL 0)
    message(CHECK_FAIL "failed (${STATUS_MESSAGE})")
    message(FATAL_ERROR "Failed to download CA certificates: ${STATUS_MESSAGE}")
endif()

message(CHECK_PASS "downloaded and verified")

set(EMBEDDED_CERT_OBJ "${CMAKE_BINARY_DIR}/cacert.o")

# Detect Architecture for objcopy
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(OBJCOPY_O_FLAG "elf64-x86-64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86)$")
    set(OBJCOPY_O_FLAG "elf32-i386")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(OBJCOPY_O_FLAG "elf64-littleaarch64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm.*|armv7.*)$")
    set(OBJCOPY_O_FLAG "elf32-littlearm")
else()
    message(FATAL_ERROR "Unsupported architecture for embedded cert: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

find_program(OBJCOPY_TOOL NAMES llvm-objcopy objcopy REQUIRED)

add_custom_command(
    OUTPUT "${EMBEDDED_CERT_OBJ}"
    COMMAND ${OBJCOPY_TOOL} -I binary -O ${OBJCOPY_O_FLAG} cacert.pem cacert.o
    COMMAND ${OBJCOPY_TOOL} --rename-section .data=.rodata,contents,alloc,load,readonly,data --set-section-alignment .rodata=8 cacert.o
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS "${CMAKE_BINARY_DIR}/cacert.pem"
    COMMENT "Embedding CA Cert via llvm-objcopy for ${CMAKE_SYSTEM_PROCESSOR}..."
)

add_custom_target(generate_cert DEPENDS "${EMBEDDED_CERT_OBJ}")

# =============================================================================
# 6. Build Target
# =============================================================================
add_executable(calyx
    src/app/main.cpp
    src/app/application.cpp
    src/core/tgz_extractor.cpp
    src/os/shell_pipe.cpp
    src/system/cpu_info.cpp
    src/system/os_info.cpp
    src/system/storage_info.cpp
    src/system/system_probe.cpp
    src/net/http_client.cpp
    src/io/uring_engine.cpp
    src/io/disk_benchmark.cpp
    src/net/speed_test.cpp
    src/ui/cli_renderer.cpp
    "${EMBEDDED_CERT_OBJ}"
)

# Enable unity build to speed up compilation times
set_target_properties(calyx PROPERTIES UNITY_BUILD ON)

# Explicit dependency
add_dependencies(calyx generate_cert)

target_include_directories(calyx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    "${GEN_INC_DIR}"
    "${GEN_INC_DIR}/include"
)

# =============================================================================
# 7. Linking
# =============================================================================
target_link_libraries(calyx 
    PRIVATE 
    CURL::libcurl
    glaze::glaze
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    liburing
    Threads::Threads
)

if(USE_LIBCXX AND STL_STATIC_LIBS)
    target_link_libraries(calyx PRIVATE ${STL_STATIC_LIBS})
endif()

# =============================================================================
# 8. Build Optimization: Precompiled Headers (PCH)
# =============================================================================

# Setup Precompiled Headers (PCH) - "The God Tier List"
target_precompile_headers(calyx PRIVATE
    # --- C++23 Heavyweights ---
    <print>
    <format>
    <filesystem>
    <ranges>
    <expected>
    
    # --- Standard Containers & Algo ---
    <string>
    <vector>
    <array>
    <map>
    <string_view>
    <span>
    <algorithm>
    <numeric>
    <bit>
    <optional>
    
    # --- Memory & Utility ---
    <memory>
    <functional>
    <utility>
    <tuple>
    
    # --- I/O & Error ---
    <fstream>
    <sstream>
    <stdexcept>
    <charconv>
    <system_error>
    
    # --- Threading ---
    <chrono>
    <thread>
    <mutex>
    <atomic>
    <stop_token>
    <future>
    
    # --- C Wrappers (POSIX) ---
    <cstdint>
    <cstring>
    <cerrno>
    <cstdlib>
    <unistd.h>
    
    # --- 3rd Party Libs ---
    <glaze/glaze.hpp>
    <curl/curl.h>
    <zlib.h>
)

log_property("Precompiled Headers" "enabled for target 'calyx'")

# =============================================================================
# 9. Install & Packaging (CPack)
# =============================================================================
install(TARGETS calyx DESTINATION bin COMPONENT bin)

set(CPACK_PACKAGE_NAME "calyx")
set(CPACK_PACKAGE_VENDOR "Alfie Ardinata")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Alfie Ardinata <alfieardinata@outlook.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/relvinarsenio/calyx")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "System Information & Network Profiling Tool")
set(CPACK_PACKAGE_DESCRIPTION "Calyx is a high-performance Linux server profiling tool written in Modern C++ (C++23).")

set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Alfie Ardinata <alfieardinata@outlook.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_COPYRIGHT "Copyright (c) 2025 Alfie Ardinata")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_STRIP_FILES TRUE)
set(CPACK_DEBIAN_COMPRESSION_TYPE "xz")

include(CPack)
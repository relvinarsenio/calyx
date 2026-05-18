cmake_minimum_required(VERSION 3.24)

# Fix FetchContent timestamp warning (CMP0135) & LTO Policy (CMP0069)
cmake_policy(SET CMP0135 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

# =============================================================================
# Calyx - Rapid Linux Server Profiler
# =============================================================================
project(calyx VERSION 1.0.1 LANGUAGES CXX C)

# Ensure all libraries and targets are built with -fPIC (Required for static-pie)
# C++23 Required
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Global settings for security and optimization
set(CMAKE_TLS_VERIFY ON CACHE BOOL "Strict TLS verification for FetchContent" FORCE)

# Global Modern CMake settings (Forced for all dependencies)
set(CMAKE_C_VISIBILITY_PRESET hidden CACHE STRING "" FORCE)
set(CMAKE_CXX_VISIBILITY_PRESET hidden CACHE STRING "" FORCE)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON CACHE BOOL "" FORCE)

# Force MinSizeRel build type globally for the project and all dependencies
set(CMAKE_BUILD_TYPE "MinSizeRel" CACHE STRING "Forced MinSizeRel" FORCE)

# =============================================================================
# 1. Compiler Detection & CCACHE SETUP (CRITICAL: MUST BE TOP)
# =============================================================================
# Enable ccache if available (Global)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "🚀 Optimization: ccache found and enabled (Global)")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DetectCompiler)

# =============================================================================
# Pre-flight Check: Ensure 'xxd' is available
# =============================================================================
find_program(XXD_EXE xxd)
if(NOT XXD_EXE)
    message(FATAL_ERROR "Error: 'xxd' tool is required but not found.")
endif()

# =============================================================================
# LLVM Full Stack Configuration
# =============================================================================
find_program(LLVM_AR llvm-ar REQUIRED)
find_program(LLVM_RANLIB llvm-ranlib REQUIRED)
find_program(LLVM_NM llvm-nm)
find_program(LLD_LINKER ld.lld REQUIRED)

set(CMAKE_AR "${LLVM_AR}" CACHE FILEPATH "Archiver" FORCE)
set(CMAKE_RANLIB "${LLVM_RANLIB}" CACHE FILEPATH "Ranlib" FORCE)
if(LLVM_NM)
    set(CMAKE_NM "${LLVM_NM}" CACHE FILEPATH "NM" FORCE)
endif()

message(STATUS "🔧 LLVM Stack:")
message(STATUS "   AR:     ${LLVM_AR}")
message(STATUS "   RANLIB: ${LLVM_RANLIB}")
message(STATUS "   LLD:    ${LLD_LINKER}")

# =============================================================================
# Global Full LTO Configuration
# =============================================================================
# CMAKE_INTERPROCEDURAL_OPTIMIZATION is handled above as a Cache variable

message(STATUS "🚀 LTO: Full LTO enabled (Optimized)")

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
    add_link_options(-Wl,-z,shstk -Wl,-z,ibt)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    add_compile_options(-mbranch-protection=standard)
    add_link_options(-Wl,-z,pac-plt -Wl,-z,force-bti)
endif()

add_link_options(
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
    -Wl,--exclude-libs,ALL
)

message(STATUS "🛡️  Security: FORTIFY_SOURCE + Stack Protector + CFI + Full RELRO + Static PIE")

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
    message(STATUS "🚀 Optimization: ${CMAKE_BUILD_TYPE} + Full LTO + GC sections")
endif()

# =============================================================================
# 2. Configure Static Dependencies
# =============================================================================
include(StaticDeps)

option(USE_IO_URING "Enable io_uring for disk benchmark" ON)

# libcurl_static implicitly inherits Full LTO from global CMake settings
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
set(CACERT_HASH "SHA256=f1407d974c5ed87d544bd931a278232e13925177e239fca370619aba63c757b4")

message(STATUS "📥 Downloading CA Certificate bundle...")

file(DOWNLOAD 
    "https://curl.se/ca/cacert-2025-12-02.pem" 
    "${CMAKE_BINARY_DIR}/cacert.pem"
    EXPECTED_HASH ${CACERT_HASH}
    TLS_VERIFY ON
    STATUS DOWNLOAD_STATUS
)

list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
list(GET DOWNLOAD_STATUS 1 STATUS_MESSAGE)

if(NOT STATUS_CODE EQUAL 0)
    message(FATAL_ERROR "❌ Failed to download CA certificates: ${STATUS_MESSAGE}")
endif()

message(STATUS "✅ CA Certificate downloaded and verified.")

# Setup Generated Paths
set(GEN_INC_DIR "${CMAKE_BINARY_DIR}/generated")
set(EMBEDDED_CERT_PATH "${GEN_INC_DIR}/include/embedded_cert.hpp")

# Ensure directory exists at Configure time
file(MAKE_DIRECTORY "${GEN_INC_DIR}/include")

add_custom_command(
    OUTPUT "${EMBEDDED_CERT_PATH}"
    # Force creation at Build time (Fix for parallel build race condition)
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_INC_DIR}/include"
    COMMAND "${XXD_EXE}" -i cacert.pem "${EMBEDDED_CERT_PATH}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS "${CMAKE_BINARY_DIR}/cacert.pem"
    COMMENT "Embedding CA Cert into header..."
)

add_custom_target(generate_cert DEPENDS "${EMBEDDED_CERT_PATH}")

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
    src/net/http_client.cpp
    src/io/disk_benchmark.cpp
    src/net/speed_test.cpp
    src/ui/cli_renderer.cpp
    "${EMBEDDED_CERT_PATH}"
)

# === OPTIMIZATION: ICF (Identical Code Folding) ===
set_target_properties(calyx PROPERTIES
    LINK_FLAGS "-Wl,--icf=all"
)

# === OPTIMIZATION: UNITY BUILD (Batch Mode) ===
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
    Threads::Threads
)

if(USE_IO_URING)
    find_path(LIBURING_INCLUDE_DIR liburing.h)
    set(_OLD_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".so")
    find_library(LIBURING_LIBRARY uring)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_OLD_SUFFIXES})
    if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
        message(STATUS "🌀 io_uring enabled")
        target_compile_definitions(calyx PRIVATE USE_IO_URING=1)
        target_include_directories(calyx PRIVATE ${LIBURING_INCLUDE_DIR})
        target_link_libraries(calyx PRIVATE ${LIBURING_LIBRARY})
    else()
        message(FATAL_ERROR "❌ liburing not found! Calyx requires io_uring for high-performance disk benchmarking. Please install liburing or use the provided build-static.sh toolchain.")
    endif()
endif()

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

message(STATUS "🚀 Optimization: Precompiled Headers (PCH) enabled for target 'calyx'")

# =============================================================================
# 9. Install & Packaging (CPack)
# =============================================================================
install(TARGETS calyx DESTINATION bin)

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
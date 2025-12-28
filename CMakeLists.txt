cmake_minimum_required(VERSION 3.24)

# Fix FetchContent timestamp warning (CMP0135)
cmake_policy(SET CMP0135 NEW)

# =============================================================================
# Bench - Modern Linux Server Benchmark
# =============================================================================
project(bench VERSION 6.9.6 LANGUAGES CXX C)

# C++23 Required
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =============================================================================
# Pre-flight Check: Ensure 'xxd' is available
# =============================================================================
find_program(XXD_EXE xxd)
if(NOT XXD_EXE)
    message(FATAL_ERROR "Error: 'xxd' tool is required but not found. Please install xxd (usually part of vim-common or xxd package) to compile this project.")
endif()

# =============================================================================
# LLVM Toolchain Detection (for Clang builds)
# =============================================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_program(LLVM_AR llvm-ar)
    find_program(LLVM_RANLIB llvm-ranlib)
    find_program(LLVM_NM llvm-nm)
    
    if(LLVM_AR)
        set(CMAKE_AR "${LLVM_AR}" CACHE FILEPATH "Archiver" FORCE)
        message(STATUS "🔧 Using llvm-ar: ${LLVM_AR}")
    endif()
    if(LLVM_RANLIB)
        set(CMAKE_RANLIB "${LLVM_RANLIB}" CACHE FILEPATH "Ranlib" FORCE)
        message(STATUS "🔧 Using llvm-ranlib: ${LLVM_RANLIB}")
    endif()
    if(LLVM_NM)
        set(CMAKE_NM "${LLVM_NM}" CACHE FILEPATH "NM" FORCE)
        message(STATUS "🔧 Using llvm-nm: ${LLVM_NM}")
    endif()
endif()

# =============================================================================
# Optimization flags (compiler-specific)
# =============================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(
        -ffunction-sections
        -fdata-sections
        -fvisibility=hidden
    )
    add_link_options(
        -Wl,--gc-sections
        -Wl,--as-needed
    )
    
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-Oz -flto)
        add_link_options(-Oz -flto)
        message(STATUS "🚀 Optimization: Clang -Oz + LTO + GC sections (size optimized)")
    else()
        add_compile_options(-O3 -flto)
        add_link_options(-O3 -flto)
        message(STATUS "🚀 Optimization: GCC -O3 + LTO + GC sections (speed optimized)")
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# =============================================================================
# 1. Smart Compiler Detection (GCC 14+ or Clang 20+)
# =============================================================================
include(DetectCompiler)

# =============================================================================
# 2. Configure Static Dependencies (ALL FROM SOURCE)
# =============================================================================
include(FetchContent)
include(StaticDeps)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
    CURL
    URL https://github.com/curl/curl/releases/download/curl-8_17_0/curl-8.17.0.tar.xz
)
FetchContent_MakeAvailable(CURL)

# =============================================================================
# 3. Compiler Configuration
# =============================================================================
add_link_options("-static")

if(USE_LIBCXX)
    message(STATUS "🔧 Mode: Clang ${DETECTED_COMPILER_VERSION} + libc++ (LLVM Full Stack)")
    add_compile_options("-stdlib=libc++")
    add_link_options("-fuse-ld=lld")
    add_link_options("-nostdlib++")
    
elseif(USE_LIBSTDCXX AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "🔧 Mode: Clang ${DETECTED_COMPILER_VERSION} + libstdc++ (GNU)")
    add_link_options("-fuse-ld=lld")
    
elseif(USE_LIBSTDCXX AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "🔧 Mode: GCC ${DETECTED_COMPILER_VERSION} + libstdc++ (Full GNU)")
endif()

# =============================================================================
# 4. Optimization Flags
# =============================================================================
add_compile_options(-O3 -Wall -Wextra -fPIE)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    add_compile_options(-march=x86-64-v3)
endif()

# =============================================================================
# 5. Build Target
# =============================================================================
add_executable(bench
    src/app/main.cpp
    src/core/interrupts.cpp
    src/core/utils.cpp
    src/os/file_descriptor.cpp
    src/os/shell_pipe.cpp
    src/system/system_info.cpp
    src/net/http_client.cpp
    src/io/disk_benchmark.cpp
    src/net/speed_test.cpp
    src/ui/cli_renderer.cpp
)

target_include_directories(bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# =============================================================================
# Embedded Certificate Generation (Standard xxd)
# =============================================================================
file(DOWNLOAD "https://curl.se/ca/cacert.pem" "${CMAKE_BINARY_DIR}/cacert.pem")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/include/embedded_cert.hpp"
    COMMAND "${XXD_EXE}" -i cacert.pem "${CMAKE_CURRENT_SOURCE_DIR}/include/embedded_cert.hpp"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS "${CMAKE_BINARY_DIR}/cacert.pem"
    COMMENT "Embedding CA Cert into header (Standard xxd)..."
)

add_custom_target(generate_cert DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/embedded_cert.hpp")
add_dependencies(bench generate_cert)
# =============================================================================
# 6. Linking
# =============================================================================
target_link_libraries(bench 
    PRIVATE 
    CURL::libcurl
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    Threads::Threads
    m
)

if(USE_LIBCXX AND STL_STATIC_LIBS)
    target_link_libraries(bench PRIVATE ${STL_STATIC_LIBS})
    
    if(EXISTS "/usr/lib/libc++abi.a")
        target_link_libraries(bench PRIVATE /usr/lib/libc++abi.a)
    endif()
elseif(USE_LIBSTDCXX)
    target_link_libraries(bench PRIVATE -static-libstdc++ -static-libgcc)
endif()

# =============================================================================
# 7. Install & Packaging (CPack)
# =============================================================================
install(TARGETS bench DESTINATION bin)

set(CPACK_PACKAGE_NAME "bench")
set(CPACK_PACKAGE_VENDOR "Alfie Ardinata")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Alfie Ardinata <alfieardinata@outlook.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/relvinarsenio/bench")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "System Information & Network Benchmark Tool")
set(CPACK_PACKAGE_DESCRIPTION "Bench is a high-performance Linux server benchmarking tool written in Modern C++ (C++23).
Features:
- Network Speed Test (Upload/Download)
- Disk I/O Benchmark
- System Information Monitoring
- Fully Static Build (No Dependencies Required)")

set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Alfie Ardinata <alfieardinata@outlook.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_STRIP_FILES TRUE)
set(CPACK_DEBIAN_COMPRESSION_TYPE "xz")

include(CPack)
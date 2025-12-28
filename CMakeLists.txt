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
# Libraries are built with -Oz for max performance (in StaticDeps.cmake)
# Final binary is also built with -Oz for consistency
# Full LTO merges everything for maximum optimization across all code
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -flto")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto -fuse-ld=lld")

message(STATUS "🚀 LTO: Full LTO enabled (all code: -Oz)")

# =============================================================================
# Security Hardening Flags (ALWAYS ON)
# =============================================================================
add_compile_options(
    # Buffer overflow protection
    -D_FORTIFY_SOURCE=2
    # Stack smashing protection
    -fstack-protector-strong
    # Prevent stack clash attacks
    -fstack-clash-protection
    # Format string vulnerability warnings
    -Wformat
    -Wformat-security
)

# Control-flow integrity (Intel CET) - x86_64 only
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    add_compile_options(-fcf-protection=full)
endif()

# Full RELRO - makes GOT read-only after relocation
add_link_options(
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
)

message(STATUS "🛡️  Security: FORTIFY_SOURCE + Stack Protector + CFI + Full RELRO")

# =============================================================================
# Optimization flags (Release)
# =============================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(
        # Maximum performance optimization
        -Oz
        # Dead code elimination
        -ffunction-sections
        -fdata-sections
        # Symbol visibility
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )
    # Linker optimizations
    add_link_options(
        -Wl,--gc-sections
        -Wl,--as-needed
    )
    message(STATUS "🚀 Optimization: -Oz + Full LTO + GC sections (performance optimized)")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# =============================================================================
# 1. Compiler Detection (Clang 20+ required)
# =============================================================================
include(DetectCompiler)

# =============================================================================
# 2. Configure Static Dependencies (ALL FROM SOURCE with LTO)
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

# Apply Full LTO with -Oz to libcurl for maximum performance
if(TARGET libcurl_static)
    target_compile_options(libcurl_static PRIVATE -flto -Oz)
endif()

# =============================================================================
# 3. Compiler Configuration (Clang + libc++ or libstdc++)
# =============================================================================
add_link_options("-static")
add_link_options("-fuse-ld=lld")

if(USE_LIBCXX)
    message(STATUS "Mode: Clang ${DETECTED_COMPILER_VERSION} + libc++ (LLVM Full Stack)")
    add_compile_options("-stdlib=libc++")
    add_link_options("-nostdlib++")
    
elseif(USE_LIBSTDCXX)
    message(STATUS "Mode: Clang ${DETECTED_COMPILER_VERSION} + libstdc++ (GNU) [fallback]")
    add_link_options("-static-libstdc++")
endif()

# =============================================================================
# 4. Warning Flags & Architecture
# =============================================================================
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wcast-qual
    -Wdouble-promotion
    -Wshadow
    -Wnull-dereference
    -Wno-unused-parameter
)

# x86-64-v3: AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT, MOVBE, XSAVE
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

# Apply ICF optimization only to the final executable
set_target_properties(bench PROPERTIES
    LINK_FLAGS "-Wl,--icf=all"
)

target_include_directories(bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# =============================================================================
# Embedded Certificate Generation
# =============================================================================
file(DOWNLOAD "https://curl.se/ca/cacert.pem" "${CMAKE_BINARY_DIR}/cacert.pem")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/include/embedded_cert.hpp"
    COMMAND "${XXD_EXE}" -i cacert.pem "${CMAKE_CURRENT_SOURCE_DIR}/include/embedded_cert.hpp"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS "${CMAKE_BINARY_DIR}/cacert.pem"
    COMMENT "Embedding CA Cert into header..."
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
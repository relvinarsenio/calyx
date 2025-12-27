cmake_minimum_required(VERSION 3.20)

# --- 1. Setup Compiler & Project ---
project(bench VERSION 6.9.6 LANGUAGES CXX C)

# Force C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 2. Dependencies (FORCE STATIC .a FILES) ---
include(FetchContent)

set(LLVM_LIB_PATH "/usr/lib/llvm-20/lib")
if(NOT EXISTS "${LLVM_LIB_PATH}/libc++.a")
    message(FATAL_ERROR "❌ libc++.a gak ketemu.")
endif()

set(STL_STATIC_LIBS 
    "${LLVM_LIB_PATH}/libc++.a"
    "${LLVM_LIB_PATH}/libc++abi.a"
    "${LLVM_LIB_PATH}/libunwind.a"
)

# --- TARGET HIJACKING: ZLIB ---
# Cari file static-nya dulu
find_library(ZLIB_STATIC_LIB 
    NAMES libz.a 
    PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib
    NO_DEFAULT_PATH
)
if(NOT ZLIB_STATIC_LIB) # Fallback
    find_library(ZLIB_STATIC_LIB NAMES libz.a)
endif()

if(NOT ZLIB_STATIC_LIB)
    message(FATAL_ERROR "❌ libz.a GAK KETEMU!")
else()
    message(STATUS "🔒 ZLIB Static Found: ${ZLIB_STATIC_LIB}")
endif()

# INI KUNCINYA: Kita bikin target ZLIB::ZLIB manual biar CURL ketipu
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB UNKNOWN IMPORTED GLOBAL)
    set_target_properties(ZLIB::ZLIB PROPERTIES
        IMPORTED_LOCATION "${ZLIB_STATIC_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "/usr/include"
    )
endif()

# Paksa variable standar CMake juga (biar makin yakin)
set(ZLIB_LIBRARY "${ZLIB_STATIC_LIB}" CACHE FILEPATH "" FORCE)
set(ZLIB_LIBRARIES "${ZLIB_STATIC_LIB}" CACHE FILEPATH "" FORCE)
set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)

# OpenSSL Static
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# A. nlohmann/json
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# B. CURL (Static)
set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "") 
set(CURL_USE_LIBPSL OFF CACHE INTERNAL "")
set(CURL_USE_LIBSSH2 OFF CACHE INTERNAL "")
set(CURL_USE_OPENSSL ON CACHE INTERNAL "") 
set(CURL_DISABLE_LDAP ON CACHE INTERNAL "") 
set(CURL_DISABLE_LDAPS ON CACHE INTERNAL "")

# Paksa CURL pake ZLIB kita
set(CURL_ZLIB ON CACHE INTERNAL "")
set(UseZLIB ON CACHE INTERNAL "")

FetchContent_Declare(
    CURL
    URL https://github.com/curl/curl/releases/download/curl-8_5_0/curl-8.5.0.tar.xz
)
FetchContent_MakeAvailable(CURL)

# --- 3. Compiler Configuration ---
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "🔧 Configuring Clang: ☢️ FULL STATIC MODE ☢️")
    add_compile_options("-stdlib=libc++")
    add_link_options("-static") 
    add_link_options("-fuse-ld=lld")
    add_link_options("-nostdlib++") 

    include(CheckIncludeFileCXX)
    set(CMAKE_REQUIRED_FLAGS "-std=c++23 -stdlib=libc++")
    check_include_file_cxx("print" CLANG_HAS_PRINT)
    unset(CMAKE_REQUIRED_FLAGS)
endif()

# --- 4. Optimization ---
add_compile_options(-O3 -Wall -Wextra -fPIE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    add_compile_options(-march=x86-64-v3)
endif()

# --- 5. Build Target ---
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

# --- 6. Linking ---
# Kita pake ZLIB::ZLIB (Target palsu yg udah kita bikin di atas)
target_link_libraries(bench 
    PRIVATE 
    CURL::libcurl
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB          # <--- Pake target, jangan path variable
    ${STL_STATIC_LIBS}   
    Threads::Threads     
    m                    
)

# --- 7. Install & Packaging ---
install(TARGETS bench DESTINATION bin)
# -- A. Metadata Dasar --
set(CPACK_PACKAGE_NAME "bench") # Nama paket di repo (cth: apt install bench-tool)
set(CPACK_PACKAGE_VENDOR "Alfie Ardinata")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Alfie Ardinata <alfieardinata@outlook.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/relvinarsenio/bench")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "System Information & Network Benchmark Tool")

# -- B. Deskripsi Panjang (Biar pro) --
# Lu bisa taruh ini di file README.txt terus pake CPACK_PACKAGE_DESCRIPTION_FILE
# Tapi gw hardcode di sini biar lu gak ribet bikin file baru.
set(CPACK_PACKAGE_DESCRIPTION "Bench is a high-performance Linux server benchmarking tool written in Modern C++ (C++23).
Features:
- Network Speed Test (Upload/Download)
- Disk I/O Benchmark
- System Information Monitoring
- Fully Static Build (No Dependencies Required)")

# -- C. Config Debian (.deb) Specific --
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Alfie Ardinata <alfieardinata@outlook.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils") # Masuk kategori 'utils'
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")

# -- D. Dependencies --
# Karena kita STATIC, kita matikan auto-dependency detection.
# Kita set manual aja ke libc6 (standar linux) buat formalitas.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)

# -- E. Strip Binary (PENTING BUAT STATIC) --
# Binary static itu gede. Opsi ini bakal buang symbol debug biar file .deb nya kecil.
set(CPACK_STRIP_FILES TRUE)
set(CPACK_DEBIAN_COMPRESSION_TYPE "xz")
include(CPack)

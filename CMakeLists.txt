cmake_minimum_required(VERSION 3.20)

# --- 1. Setup Compiler ---
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(GCC_PATH "g++")
    if(GCC_PATH)
        set(CMAKE_CXX_COMPILER "${GCC_PATH}")
    endif()
endif()

project(bench VERSION 1.0 LANGUAGES CXX)

# --- 2. Dependencies ---
find_package(CURL REQUIRED)

# nlohmann/json dependency from source (GitHub)
include(FetchContent)
FetchContent_Declare(
    json 
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

# --- 3. Force Standard C++23 ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# --- 4. STRICT CHECK: Wajib ada <print> ---
include(CheckIncludeFileCXX)
check_include_file_cxx("print" HAVE_STD_PRINT)

if(NOT HAVE_STD_PRINT)
    message(FATAL_ERROR "❌ ERROR: Header <print> tidak ditemukan!\n"
    "Harap gunakan GCC 14+ atau Clang 18+ dengan libc++ terbaru.")
endif()

# --- 5. Flags (Optimasi & Security) ---
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -O3 -march=x86-64-v3 -flto=auto
        -Wall -Wextra
        -fstack-protector-strong -fcf-protection -fPIE
        -D_FORTIFY_SOURCE=2
        -Wformat -Werror=format-security
    )
    add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,--gc-sections -pie)
endif()

# --- 6. Build ---
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

# Link ke CURL dan nlohmann_json
target_link_libraries(bench PRIVATE CURL::libcurl nlohmann_json::nlohmann_json m)
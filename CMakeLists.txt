cmake_minimum_required(VERSION 3.24)

# --- 1. Setup Compiler ---
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(GCC_PATH "g++")
    if(GCC_PATH)
        set(CMAKE_CXX_COMPILER "${GCC_PATH}")
    endif()
endif()

project(bench VERSION 6.9.6 LANGUAGES CXX)

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

# --- 4. Compiler Specific Configuration ---
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    include(CheckIncludeFileCXX)
    
    # 1. Cek dulu apakah library bawaan sistem (libstdc++) support <print>
    message(STATUS "🔍 Checking system stdlib support for <print>...")
    check_include_file_cxx("print" CLANG_HAS_SYSTEM_PRINT)

    if (CLANG_HAS_SYSTEM_PRINT)
        message(STATUS "✅ Clang: System libstdc++ supports <print>. Using system defaults.")
        # Gunakan LLD agar support LTO (Clang bitcode)
        add_link_options("-fuse-ld=lld") 
    else()
        message(STATUS "⚠️ Clang: System libstdc++ does NOT support <print>.")
        message(STATUS "🔹 Switching to LLVM Full Stack (libc++, lld)...")
        
        # Prepare flags for libc++
        set(LIBCXX_FLAGS "-stdlib=libc++")
        set(LIBCXX_LINKER_FLAGS "-stdlib=libc++" "-fuse-ld=lld")

        # Attempt to find LLVM library path relative to compiler
        get_filename_component(COMPILER_BIN ${CMAKE_CXX_COMPILER} REALPATH)
        get_filename_component(COMPILER_DIR ${COMPILER_BIN} DIRECTORY)
        get_filename_component(LLVM_ROOT ${COMPILER_DIR} DIRECTORY)
        
        set(LLVM_LIB_DIR "${LLVM_ROOT}/lib")
        
        if(EXISTS "${LLVM_LIB_DIR}/libc++.so")
            message(STATUS "   Using LLVM libraries from: ${LLVM_LIB_DIR}")
            link_directories(${LLVM_LIB_DIR})
            list(APPEND LIBCXX_LINKER_FLAGS "-Wl,-rpath,${LLVM_LIB_DIR}" "-lc++" "-lc++abi")
        endif()

        # 2. Double Check: Apakah libc++ ini support <print>?
        unset(CLANG_HAS_LIBCXX_PRINT CACHE)
        set(CMAKE_REQUIRED_FLAGS "${LIBCXX_FLAGS}")
        check_include_file_cxx("print" CLANG_HAS_LIBCXX_PRINT)
        unset(CMAKE_REQUIRED_FLAGS)

        if(CLANG_HAS_LIBCXX_PRINT)
             message(STATUS "✅ Clang: LLVM libc++ supports <print>. Proceeding with Full Stack.")
             add_compile_options(${LIBCXX_FLAGS})
             add_link_options(${LIBCXX_LINKER_FLAGS})
        else()
             message(FATAL_ERROR "❌ CRITICAL ERROR: \n"
             "   1. System libstdc++ does not support <print>.\n"
             "   2. LLVM libc++ also does not support <print>.\n"
             "   Solution: Please upgrade to Clang 20+ (with libc++ 20+) or GCC 14+.")
        endif()
    endif()
    
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    message(STATUS "🔸 GCC detected: Using default GNU stack")
    
    # Validate GCC support for <print>
    include(CheckIncludeFileCXX)
    check_include_file_cxx("print" GCC_HAS_PRINT)
    if(NOT GCC_HAS_PRINT)
        message(FATAL_ERROR "❌ ERROR: GCC version too old. Header <print> not found.\n"
                            "   Please upgrade to GCC 14+.")
    endif()
endif()

# --- 5. Flags (Optimasi & Security) ---

# --- 5. Flags (Optimasi & Security) ---
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -O3
        -Wall -Wextra
        -fstack-protector-strong -fPIE
        -D_FORTIFY_SOURCE=2
        -Wformat -Werror=format-security
    )
    
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        add_compile_options(-march=x86-64-v3 -fcf-protection)
    endif()

    add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,--gc-sections -pie)
endif()

# --- 7. LTO / IPO Configuration ---
include(CheckIPOSupported)
check_ipo_supported(RESULT result OUTPUT output)
if(result)
    message(STATUS "🚀 IPO/LTO is supported: Enabling globally")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(WARNING "⚠️ IPO/LTO is not supported: ${output}")
endif()

# --- 8. Build ---
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

# --- 9. Install & Packaging (CPack) ---
install(TARGETS bench DESTINATION bin)

set(CPACK_PACKAGE_NAME "bench")
set(CPACK_PACKAGE_VENDOR "Alfie Ardinata")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Modern Linux Server Benchmark Tool (C++23)")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Alfie Ardinata")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES TRUE)

# DEB Generator Settings
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Alfie Ardinata")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON) # Auto-detect dependencies
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

# RPM Generator Settings
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON) # Auto-detect dependencies

# Generators to use
set(CPACK_GENERATOR "TGZ;DEB;RPM")

include(CPack)

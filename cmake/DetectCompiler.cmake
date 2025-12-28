# =============================================================================
# DetectCompiler.cmake - Clang + libc++ Compiler Detection for C++23 Static Builds
# =============================================================================
# Requires Clang 20+ with:
#   1. libc++ (primary) - LLVM's stdlib with full C++23 support
#   2. libstdc++ (fallback) - GNU stdlib if libc++ unavailable
#
# This project requires Docker build for consistent results.
# =============================================================================

set(BENCH_MIN_CLANG_VERSION 20)

# =============================================================================
# Verify Clang is being used
# =============================================================================
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR 
        "❌ This project requires Clang!\n"
        "\n"
        "Detected compiler: ${CMAKE_CXX_COMPILER_ID}\n"
        "\n"
        "Please use Docker to build:\n"
        "   ./build-static.sh\n"
        "\n"
        "Or set Clang manually:\n"
        "   CC=clang CXX=clang++ cmake ...\n"
    )
endif()

# =============================================================================
# Helper: Test if std::print works with current compiler/stdlib
# =============================================================================
function(test_std_print_support OUT_RESULT)
    set(TEST_CODE "
#include <print>
void test() { std::print(\"test\"); }
")
    
    include(CheckCXXSourceCompiles)
    
    set(CMAKE_REQUIRED_FLAGS_BACKUP "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE_BACKUP "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(CMAKE_REQUIRED_FLAGS "-std=c++23 ${EXTRA_CXX_FLAGS}")
    
    check_cxx_source_compiles("${TEST_CODE}" ${OUT_RESULT})
    
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_BACKUP}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${CMAKE_TRY_COMPILE_TARGET_TYPE_BACKUP}")
    
    set(${OUT_RESULT} ${${OUT_RESULT}} PARENT_SCOPE)
endfunction()

# =============================================================================
# Helper: Find libc++ static libraries
# =============================================================================
function(find_libcxx_static OUT_PATH OUT_LIBS)
    set(SEARCH_PATHS
        "/usr/lib/llvm-20/lib"
        "/usr/lib/llvm-21/lib"
        "/usr/lib/llvm-19/lib"
        "/usr/lib"
        "/usr/local/lib"
        "/opt/llvm/lib"
    )

    set(FOUND_PATH "")
    foreach(LIB_PATH ${SEARCH_PATHS})
        if(EXISTS "${LIB_PATH}/libc++.a")
            set(FOUND_PATH "${LIB_PATH}")
            break()
        endif()
    endforeach()

    if(NOT FOUND_PATH)
        set(${OUT_PATH} "" PARENT_SCOPE)
        set(${OUT_LIBS} "" PARENT_SCOPE)
        return()
    endif()

    set(STATIC_LIBS "${FOUND_PATH}/libc++.a")
    
    if(EXISTS "${FOUND_PATH}/libc++abi.a")
        list(APPEND STATIC_LIBS "${FOUND_PATH}/libc++abi.a")
    endif()
    
    if(EXISTS "${FOUND_PATH}/libunwind.a")
        list(APPEND STATIC_LIBS "${FOUND_PATH}/libunwind.a")
    endif()

    set(${OUT_PATH} "${FOUND_PATH}" PARENT_SCOPE)
    set(${OUT_LIBS} "${STATIC_LIBS}" PARENT_SCOPE)
endfunction()

# =============================================================================
# Main Detection Logic
# =============================================================================

set(COMPILER_DETECTED FALSE)
set(USE_LIBCXX FALSE)
set(USE_LIBSTDCXX FALSE)
set(STL_STATIC_LIBS "")

# Extract Clang version
execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} --version
    OUTPUT_VARIABLE CLANG_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "clang version ([0-9]+)" _ "${CLANG_VERSION_OUTPUT}")
set(CLANG_VERSION ${CMAKE_MATCH_1})

if(NOT CLANG_VERSION VERSION_GREATER_EQUAL ${BENCH_MIN_CLANG_VERSION})
    message(FATAL_ERROR 
        "❌ Clang version ${CLANG_VERSION} detected, minimum required is ${BENCH_MIN_CLANG_VERSION}.\n"
        "\n"
        "Please use Docker to build:\n"
        "   ./build-static.sh\n"
    )
endif()

# -----------------------------------------------------------------------------
# Option 1: Try Clang with libc++ (Primary - preferred for static builds)
# -----------------------------------------------------------------------------
message(STATUS "🔍 Testing Clang ${CLANG_VERSION} with libc++...")

find_libcxx_static(LLVM_LIB_PATH STL_STATIC_LIBS)

if(LLVM_LIB_PATH)
    set(EXTRA_CXX_FLAGS "-stdlib=libc++")
    test_std_print_support(CLANG_LIBCXX_HAS_PRINT)
    
    if(CLANG_LIBCXX_HAS_PRINT)
        message(STATUS "✅ Clang ${CLANG_VERSION} with libc++ supports std::print!")
        set(COMPILER_DETECTED TRUE)
        set(USE_LIBCXX TRUE)
        set(DETECTED_COMPILER "Clang")
        set(DETECTED_COMPILER_VERSION "${CLANG_VERSION}")
        message(STATUS "🔧 Found libc++ at: ${LLVM_LIB_PATH}")
    else()
        message(STATUS "⚠️  Clang ${CLANG_VERSION} with libc++ lacks std::print")
    endif()
else()
    message(STATUS "⚠️  libc++ static libraries not found, trying libstdc++...")
endif()

# -----------------------------------------------------------------------------
# Option 2: Try Clang with libstdc++ (Fallback)
# -----------------------------------------------------------------------------
if(NOT COMPILER_DETECTED)
    message(STATUS "🔍 Testing Clang ${CLANG_VERSION} with libstdc++ (fallback)...")
    
    set(EXTRA_CXX_FLAGS "")
    test_std_print_support(CLANG_LIBSTDCXX_HAS_PRINT)
    
    if(CLANG_LIBSTDCXX_HAS_PRINT)
        message(STATUS "✅ Clang ${CLANG_VERSION} with libstdc++ supports std::print!")
        set(COMPILER_DETECTED TRUE)
        set(USE_LIBSTDCXX TRUE)
        set(DETECTED_COMPILER "Clang")
        set(DETECTED_COMPILER_VERSION "${CLANG_VERSION}")
    else()
        message(STATUS "⚠️  Clang ${CLANG_VERSION} with libstdc++ lacks std::print")
    endif()
endif()

# -----------------------------------------------------------------------------
# Final Check
# -----------------------------------------------------------------------------
if(NOT COMPILER_DETECTED)
    message(FATAL_ERROR 
        "❌ Clang ${CLANG_VERSION} found but no suitable stdlib!\n"
        "\n"
        "This project requires std::print support.\n"
        "\n"
        "Please use Docker to build (recommended):\n"
        "   ./build-static.sh\n"
        "\n"
        "Docker provides a consistent build environment with:\n"
        "   - Clang 20+\n"
        "   - libc++ with full C++23 support\n"
        "   - All dependencies built from source\n"
    )
endif()

# =============================================================================
# Export Results
# =============================================================================
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "  Compiler: ${DETECTED_COMPILER} ${DETECTED_COMPILER_VERSION}")
if(USE_LIBCXX)
    message(STATUS "  Stdlib:   libc++ (LLVM)")
else()
    message(STATUS "  Stdlib:   libstdc++ (GNU) [fallback]")
endif()
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "")

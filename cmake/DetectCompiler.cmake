# =============================================================================
# DetectCompiler.cmake - C++23 Toolchain & Standard Library Validation
# =============================================================================
# Mandates Clang >= 20 or GCC >= 14 with full C++23 std::print and std::expected.
# =============================================================================

# Enforce minimum toolchain versions supporting ISO C++23 core features
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
    message(FATAL_ERROR "❌ Clang 20+ required for C++23 features. Detected: Clang ${CMAKE_CXX_COMPILER_VERSION}.\nPlease use Docker: ./build-static.sh")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "❌ GCC 14+ required for C++23 features. Detected: GCC ${CMAKE_CXX_COMPILER_VERSION}.\nPlease use Docker: ./build-static.sh")
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "❌ Unsupported compiler family: ${CMAKE_CXX_COMPILER_ID}. Calyx requires Clang >= 20 or GCC >= 14.")
endif()

# Include CMake native symbol check module
include(CheckCXXSymbolExists)

if(POLICY CMP0140)
    cmake_policy(SET CMP0140 NEW)
endif()

# Isolate C++23 symbol verification variables using CMake 3.25+ block scoping
block(SCOPE_FOR VARIABLES PROPAGATE HAS_STD_PRINT HAS_STD_EXPECTED HAS_STD_FORMAT USE_LIBCXX STL_STATIC_LIBS)
    set(CMAKE_REQUIRED_FLAGS "-std=c++23")

    check_cxx_symbol_exists("__cpp_lib_print" "print;version" HAS_STD_PRINT)
    check_cxx_symbol_exists("__cpp_lib_expected" "expected;version" HAS_STD_EXPECTED)
    check_cxx_symbol_exists("__cpp_lib_format" "format;version" HAS_STD_FORMAT)

    # Fallback to LLVM libc++ when system libstdc++ lacks C++23 std::print/std::format implementation
    if((NOT HAS_STD_PRINT OR NOT HAS_STD_EXPECTED OR NOT HAS_STD_FORMAT) AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(CMAKE_REQUIRED_FLAGS "-std=c++23 -stdlib=libc++")
        check_cxx_symbol_exists("__cpp_lib_print" "print;version" HAS_STD_PRINT)
        check_cxx_symbol_exists("__cpp_lib_expected" "expected;version" HAS_STD_EXPECTED)
        check_cxx_symbol_exists("__cpp_lib_format" "format;version" HAS_STD_FORMAT)
        if(HAS_STD_PRINT AND HAS_STD_EXPECTED AND HAS_STD_FORMAT)
            set(USE_LIBCXX TRUE)
        endif()
    endif()

    if(USE_LIBCXX)
        find_library(LIBCXX_A NAMES libc++.a PATHS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES} NO_DEFAULT_PATH)
        find_library(LIBCXXABI_A NAMES libc++abi.a PATHS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES} NO_DEFAULT_PATH)
        find_library(LIBUNWIND_A NAMES libunwind.a unwind.a PATHS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES} NO_DEFAULT_PATH)
        
        if(LIBCXX_A)
            list(APPEND STL_STATIC_LIBS "${LIBCXX_A}")
        endif()
        if(LIBCXXABI_A)
            list(APPEND STL_STATIC_LIBS "${LIBCXXABI_A}")
        endif()
        if(LIBUNWIND_A)
            list(APPEND STL_STATIC_LIBS "${LIBUNWIND_A}")
        endif()
    endif()
endblock()

if(NOT HAS_STD_PRINT OR NOT HAS_STD_EXPECTED OR NOT HAS_STD_FORMAT)
    message(FATAL_ERROR "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} lacks standard library C++23 <print>, <expected>, or <format> support.\nPlease use Docker: ./build-static.sh")
endif()

log_section("Compiler & C++23 Standard Library")
log_property("Compiler" "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(USE_LIBCXX)
    log_property("Stdlib" "libc++ (LLVM)")
else()
    log_property("Stdlib" "libstdc++ (GNU)")
endif()

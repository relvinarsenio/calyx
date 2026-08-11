# =============================================================================
# StaticDeps.cmake - Building Static Dependencies with LTO
# =============================================================================
# All 3rd-party static dependencies (zlib, LibreSSL, libcurl, glaze, liburing)
# are compiled directly from source using FetchContent.
# LLVM stack with LTO on all libraries, final binary optimized.
# =============================================================================

include(FetchContent)

# Detect number of CPUs for parallel build
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 1)
endif()

# =============================================================================
# Centralized 3rd-party dependency versions and SHA256 checksums
# =============================================================================
set(ZLIB_DEP_VERSION     "1.3.2")
set(ZLIB_DEP_HASH        "SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16")

set(LIBRESSL_DEP_VERSION "4.3.2")
set(LIBRESSL_DEP_HASH    "SHA256=edf01aee24c65d69e6a9efcb9d44bcda682ff9d4f3bbbd95e794e1dfa90847b5")

set(CURL_DEP_VERSION     "8.21.0")
set(CURL_DEP_TAG         "8_21_0")
set(CURL_DEP_HASH        "SHA256=aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6")

set(GLAZE_DEP_VERSION    "7.8.4")
set(GLAZE_DEP_HASH       "SHA256=65331a8f8ffa56a3c5990eb31119db8afc1cec0b48de57b4ebbc9c1286ea74a3")

set(LIBURING_DEP_VERSION "2.15")
set(LIBURING_DEP_HASH    "SHA256=8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a")

# =============================================================================
# 1. ZLIB - Build from source with LTO
# =============================================================================
message(CHECK_START "Fetching dependency: zlib")

set(ZLIB_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(ZLIB_BUILD_SHARED OFF CACHE INTERNAL "Disable zlib shared library")
set(ZLIB_BUILD_TESTING OFF CACHE INTERNAL "Disable zlib tests")
set(SKIP_INSTALL_ALL ON CACHE INTERNAL "")

FetchContent_Declare(
    zlib
    URL "https://github.com/madler/zlib/releases/download/v${ZLIB_DEP_VERSION}/zlib-${ZLIB_DEP_VERSION}.tar.gz"
    URL_HASH "${ZLIB_DEP_HASH}"
)

FetchContent_MakeAvailable(zlib)

# Zlib implicitly inherits LTO from global CMake settings
get_target_property(ZLIBSTATIC_LOCATION zlibstatic ARCHIVE_OUTPUT_DIRECTORY)
if(NOT ZLIBSTATIC_LOCATION)
    set(ZLIBSTATIC_LOCATION "${zlib_BINARY_DIR}")
endif()
set(ZLIB_LIBRARY_PATH "${ZLIBSTATIC_LOCATION}/libz.a")

if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB STATIC IMPORTED GLOBAL)
    set_target_properties(ZLIB::ZLIB PROPERTIES
        IMPORTED_LOCATION "${ZLIB_LIBRARY_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}"
    )
    add_dependencies(ZLIB::ZLIB zlibstatic)
endif()

set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
set(ZLIB_INCLUDE_DIRS "${ZLIB_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY "${ZLIB_LIBRARY_PATH}" CACHE STRING "" FORCE)
set(ZLIB_LIBRARIES "${ZLIB_LIBRARY_PATH}" CACHE STRING "" FORCE)

include_directories(SYSTEM ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
message(CHECK_PASS "built from source with LTO (v${ZLIB_DEP_VERSION})")

# =============================================================================
# 2. LibreSSL - Build from source using FetchContent with LTO
# =============================================================================
message(CHECK_START "Fetching dependency: LibreSSL")

# LibreSSL build options - disable everything we don't need
set(LIBRESSL_APPS OFF CACHE INTERNAL "Don't build LibreSSL apps (openssl, ocspcheck)")
set(LIBRESSL_TESTS OFF CACHE INTERNAL "Don't build LibreSSL tests")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "Build static libraries only")

# Disable optional features
set(ENABLE_ASM ON CACHE INTERNAL "Enable assembly optimizations for performance")
set(ENABLE_EXTRATESTS OFF CACHE INTERNAL "Disable extra tests")
set(ENABLE_NC OFF CACHE INTERNAL "Disable netcat utility")

FetchContent_Declare(
    libressl
    URL "https://cloudflare.cdn.openbsd.org/pub/OpenBSD/LibreSSL/libressl-${LIBRESSL_DEP_VERSION}.tar.gz"
    URL_HASH "${LIBRESSL_DEP_HASH}"
)

FetchContent_MakeAvailable(libressl)

# LibreSSL targets implicitly inherit LTO from global CMake settings

# Get library output directories
get_target_property(CRYPTO_OUTPUT_DIR crypto ARCHIVE_OUTPUT_DIRECTORY)
if(NOT CRYPTO_OUTPUT_DIR)
    set(CRYPTO_OUTPUT_DIR "${libressl_BINARY_DIR}/crypto")
endif()

get_target_property(SSL_OUTPUT_DIR ssl ARCHIVE_OUTPUT_DIRECTORY)
if(NOT SSL_OUTPUT_DIR)
    set(SSL_OUTPUT_DIR "${libressl_BINARY_DIR}/ssl")
endif()

set(OPENSSL_CRYPTO_LIBRARY "${CRYPTO_OUTPUT_DIR}/libcrypto.a")
set(OPENSSL_SSL_LIBRARY "${SSL_OUTPUT_DIR}/libssl.a")

# Create OpenSSL::Crypto IMPORTED target
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${libressl_SOURCE_DIR}/include"
)
add_dependencies(OpenSSL::Crypto crypto)

# Create OpenSSL::SSL IMPORTED target
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${libressl_SOURCE_DIR}/include"
    INTERFACE_LINK_LIBRARIES "OpenSSL::Crypto"
)
add_dependencies(OpenSSL::SSL ssl)

# Set CMake variables for compatibility (curl expects OPENSSL_* variables)
set(OPENSSL_FOUND TRUE CACHE BOOL "" FORCE)
set(OPENSSL_INCLUDE_DIR "${libressl_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_CRYPTO_LIBRARY}" CACHE FILEPATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "${OPENSSL_SSL_LIBRARY}" CACHE FILEPATH "" FORCE)
set(OPENSSL_ROOT_DIR "${libressl_SOURCE_DIR}" CACHE PATH "" FORCE)
set(OPENSSL_VERSION "${LIBRESSL_DEP_VERSION}" CACHE STRING "" FORCE)

# Make headers available
include_directories(SYSTEM "${libressl_SOURCE_DIR}/include")

message(CHECK_PASS "built from source with LTO (v${LIBRESSL_DEP_VERSION})")

# =============================================================================
# 3. Threads (this MUST come from OS - it's part of libc)
# =============================================================================
find_package(Threads REQUIRED)

# =============================================================================
# 4. CURL Configuration - ULTRA MINIMAL BUILD (HTTP/HTTPS only) with LTO
# =============================================================================
message(CHECK_START "Fetching dependency: libcurl")

# Build options
set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE INTERNAL "")

# SSL/TLS - Required for HTTPS
set(CURL_USE_OPENSSL ON CACHE INTERNAL "")
set(CURL_DISABLE_OPENSSL_AUTO_LOAD_CONFIG ON CACHE INTERNAL "Disable OpenSSL auto configuration loading")

# Compression
set(CURL_ZLIB ON CACHE INTERNAL "")

# Disable optional features
set(CURL_USE_LIBPSL OFF CACHE INTERNAL "")
set(CURL_USE_LIBSSH2 OFF CACHE INTERNAL "")
set(CURL_USE_LIBSSH OFF CACHE INTERNAL "")
set(USE_LIBIDN2 OFF CACHE INTERNAL "")
set(CURL_USE_GSSAPI OFF CACHE INTERNAL "")
set(ENABLE_ARES OFF CACHE INTERNAL "")
set(CURL_BROTLI OFF CACHE INTERNAL "")
set(CURL_ZSTD OFF CACHE INTERNAL "")
set(CURL_DISABLE_ALTSVC ON CACHE INTERNAL "")
set(CURL_DISABLE_COOKIES OFF CACHE INTERNAL "")
set(CURL_DISABLE_HSTS ON CACHE INTERNAL "")
set(CURL_DISABLE_HTTP_AUTH OFF CACHE INTERNAL "")
set(CURL_DISABLE_NETRC ON CACHE INTERNAL "")
set(CURL_DISABLE_PARSEDATE ON CACHE INTERNAL "")
set(CURL_DISABLE_PROGRESS_METER ON CACHE INTERNAL "")
set(CURL_DISABLE_SHUFFLE_DNS ON CACHE INTERNAL "")
set(CURL_DISABLE_SOCKETPAIR ON CACHE INTERNAL "")
set(CURL_DISABLE_VERBOSE_STRINGS OFF CACHE INTERNAL "Keep verbose error messages for debugging")
set(CURL_DISABLE_NTLM ON CACHE INTERNAL "")
set(CURL_DISABLE_GETOPTIONS ON CACHE INTERNAL "")
set(CURL_DISABLE_BINDLOCAL ON CACHE INTERNAL "")
set(CURL_DISABLE_DOH ON CACHE INTERNAL "")
set(CURL_DISABLE_MIME ON CACHE INTERNAL "")
set(CURL_DISABLE_FORM_API ON CACHE INTERNAL "")
set(CURL_DISABLE_HEADERS_API ON CACHE INTERNAL "")

# Disable ALL protocols except HTTP/HTTPS
set(CURL_DISABLE_FTP ON CACHE INTERNAL "")
set(CURL_DISABLE_FTPS ON CACHE INTERNAL "")
set(CURL_DISABLE_GOPHER ON CACHE INTERNAL "")
set(CURL_DISABLE_IMAP ON CACHE INTERNAL "")
set(CURL_DISABLE_LDAP ON CACHE INTERNAL "")
set(CURL_DISABLE_LDAPS ON CACHE INTERNAL "")
set(CURL_DISABLE_MQTT ON CACHE INTERNAL "")
set(CURL_DISABLE_POP3 ON CACHE INTERNAL "")
set(CURL_DISABLE_RTSP ON CACHE INTERNAL "")
set(CURL_DISABLE_SMB ON CACHE INTERNAL "")
set(CURL_DISABLE_SMTP ON CACHE INTERNAL "")
set(CURL_DISABLE_TELNET ON CACHE INTERNAL "")
set(CURL_DISABLE_TFTP ON CACHE INTERNAL "")
set(CURL_DISABLE_DICT ON CACHE INTERNAL "")
set(CURL_DISABLE_FILE ON CACHE INTERNAL "")

# Disable more legacy/unused features
set(CURL_DISABLE_PROXY OFF CACHE INTERNAL "Keep proxy support")
set(CURL_DISABLE_BASIC_AUTH OFF CACHE INTERNAL "Keep basic auth for API calls")
set(CURL_DISABLE_BEARER_AUTH ON CACHE INTERNAL "Disable bearer auth")
set(CURL_DISABLE_DIGEST_AUTH ON CACHE INTERNAL "Disable digest auth")
set(CURL_DISABLE_KERBEROS_AUTH ON CACHE INTERNAL "Disable kerberos")
set(CURL_DISABLE_NEGOTIATE_AUTH ON CACHE INTERNAL "Disable negotiate auth")
set(CURL_DISABLE_AWS ON CACHE INTERNAL "Disable AWS sigv4")
set(CURL_DISABLE_IPFS ON CACHE INTERNAL "Disable IPFS")

# IPv6 support for libcurl
set(ENABLE_IPV6 ON CACHE BOOL "Enable IPv6 in libcurl" FORCE)
set(CURL_DISABLE_IPV6 OFF CACHE BOOL "Keep IPv6 support in libcurl" FORCE)

# CA bundle
set(CURL_CA_BUNDLE "auto" CACHE STRING "" FORCE)
set(CURL_CA_PATH "auto" CACHE STRING "" FORCE)

FetchContent_Declare(
    CURL
    URL "https://github.com/curl/curl/releases/download/curl-${CURL_DEP_TAG}/curl-${CURL_DEP_VERSION}.tar.xz"
    URL_HASH "${CURL_DEP_HASH}"
)
FetchContent_MakeAvailable(CURL)
message(CHECK_PASS "ultra minimal build with LTO (v${CURL_DEP_VERSION})")

# =============================================================================
# 5. glaze - Build from source
# =============================================================================
message(CHECK_START "Fetching dependency: glaze")

FetchContent_Declare(
    glaze
    URL "https://github.com/stephenberry/glaze/archive/refs/tags/v${GLAZE_DEP_VERSION}.tar.gz"
    URL_HASH "${GLAZE_DEP_HASH}"
)
FetchContent_MakeAvailable(glaze)
message(CHECK_PASS "header-only JSON library (v${GLAZE_DEP_VERSION})")

# =============================================================================
# 6. liburing - Build static library from source with LTO
# =============================================================================
message(CHECK_START "Fetching dependency: liburing")

FetchContent_GetProperties(liburing)
if(NOT liburing_POPULATED)
    FetchContent_Populate(
        liburing
        URL "https://github.com/axboe/liburing/archive/refs/tags/liburing-${LIBURING_DEP_VERSION}.tar.gz"
        URL_HASH "${LIBURING_DEP_HASH}"
        DOWNLOAD_NO_PROGRESS TRUE
    )
    if(NOT EXISTS "${liburing_SOURCE_DIR}/src/include/liburing/compat.h")
        file(WRITE "${liburing_SOURCE_DIR}/src/include/liburing/compat.h"
            "/* SPDX-License-Identifier: MPL-2.0 */\n"
            "#ifndef LIBURING_COMPAT_H\n"
            "#define LIBURING_COMPAT_H\n\n"
            "#if defined(__has_include)\n"
            "#if __has_include(\"linux/time_types.h\")\n"
            "#include <linux/time_types.h>\n"
            "#else\n"
            "struct __kernel_timespec {\n"
            "\tint64_t tv_sec;\n"
            "\tlong long tv_nsec;\n"
            "};\n"
            "#endif\n"
            "#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1\n"
            "#endif\n\n"
            "#if !defined(__has_include)\n"
            "#include <linux/time_types.h>\n"
            "#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1\n"
            "#endif\n\n"
            "#include <linux/openat2.h>\n"
            "#include <sys/stat.h>\n"
            "#include <linux/blkdev.h>\n\n"
            "#endif\n"
        )
    endif()
    if(NOT EXISTS "${liburing_SOURCE_DIR}/src/include/liburing/io_uring_version.h")
        string(REPLACE "." ";" _liburing_ver_list "${LIBURING_DEP_VERSION}")
        list(GET _liburing_ver_list 0 _liburing_major)
        list(GET _liburing_ver_list 1 _liburing_minor)
        file(WRITE "${liburing_SOURCE_DIR}/src/include/liburing/io_uring_version.h"
            "/* SPDX-License-Identifier: MPL-2.0 */\n"
            "#ifndef LIBURING_VERSION_H\n"
            "#define LIBURING_VERSION_H\n\n"
            "#define IO_URING_VERSION_MAJOR ${_liburing_major}\n"
            "#define IO_URING_VERSION_MINOR ${_liburing_minor}\n\n"
            "#endif\n"
        )
    endif()
    file(GLOB LIBURING_SRCS "${liburing_SOURCE_DIR}/src/*.c")
    list(REMOVE_ITEM LIBURING_SRCS
        "${liburing_SOURCE_DIR}/src/nolibc.c"
        "${liburing_SOURCE_DIR}/src/sanitize.c"
        "${liburing_SOURCE_DIR}/src/ffi.c"
    )
    add_library(liburing STATIC ${LIBURING_SRCS})
    target_include_directories(liburing
        SYSTEM PUBLIC "${liburing_SOURCE_DIR}/src/include"
        PRIVATE "${liburing_SOURCE_DIR}/src"
    )
    target_compile_definitions(liburing PRIVATE _GNU_SOURCE)
    set_target_properties(liburing PROPERTIES
        OUTPUT_NAME "uring"
        POSITION_INDEPENDENT_CODE ON
        UNITY_BUILD ON
        ARCHIVE_OUTPUT_DIRECTORY "${liburing_BINARY_DIR}"
    )
endif()

set(LIBURING_INCLUDE_DIR "${liburing_SOURCE_DIR}/src/include" CACHE PATH "" FORCE)
set(LIBURING_LIBRARY liburing CACHE STRING "" FORCE)
message(CHECK_PASS "built static from source with LTO (v${LIBURING_DEP_VERSION})")

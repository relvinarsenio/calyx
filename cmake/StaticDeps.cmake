# =============================================================================
# StaticDeps.cmake - Static Dependencies Configuration (ALL FROM SOURCE)
# =============================================================================
# Builds ALL dependencies from source for fully reproducible static builds.
# Full LLVM stack with Full LTO (-Oz) on all libraries, final binary with -Oz.
# =============================================================================

include(FetchContent)

# Detect number of CPUs for parallel build
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 1)
endif()

# =============================================================================
# 1. ZLIB - Build from source with LTO
# =============================================================================
message(STATUS "📦 Fetching zlib...")

set(ZLIB_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(ZLIB_BUILD_SHARED OFF CACHE INTERNAL "Disable zlib shared library")
set(SKIP_INSTALL_ALL ON CACHE INTERNAL "")

FetchContent_Declare(
    zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
)

FetchContent_MakeAvailable(zlib)

# Apply Full LTO and -Oz to zlib for maximum performance
target_compile_options(zlibstatic PRIVATE -flto -Oz)

# Get the actual library path for zlibstatic
get_target_property(ZLIBSTATIC_LOCATION zlibstatic ARCHIVE_OUTPUT_DIRECTORY)
if(NOT ZLIBSTATIC_LOCATION)
    set(ZLIBSTATIC_LOCATION "${zlib_BINARY_DIR}")
endif()
set(ZLIB_LIBRARY_PATH "${ZLIBSTATIC_LOCATION}/libz.a")

# Create ZLIB::ZLIB as IMPORTED target for full CMake compatibility
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB STATIC IMPORTED GLOBAL)
    set_target_properties(ZLIB::ZLIB PROPERTIES
        IMPORTED_LOCATION "${ZLIB_LIBRARY_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}"
    )
    add_dependencies(ZLIB::ZLIB zlibstatic)
endif()

# Set variables for other CMake scripts
set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
set(ZLIB_INCLUDE_DIRS "${ZLIB_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY "${ZLIB_LIBRARY_PATH}" CACHE STRING "" FORCE)
set(ZLIB_LIBRARIES "${ZLIB_LIBRARY_PATH}" CACHE STRING "" FORCE)

include_directories(SYSTEM ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

message(STATUS "🔒 ZLIB: Building from source with Full LTO -Oz (v1.3.1)")

# =============================================================================
# 2. LibreSSL - Build from source using FetchContent with LTO
# =============================================================================
message(STATUS "📦 Fetching LibreSSL...")

# LibreSSL build options - disable everything we don't need
set(LIBRESSL_APPS OFF CACHE INTERNAL "Don't build LibreSSL apps (openssl, ocspcheck)")
set(LIBRESSL_TESTS OFF CACHE INTERNAL "Don't build LibreSSL tests")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "Build static libraries only")

# Disable optional features
set(ENABLE_ASM ON CACHE INTERNAL "Enable assembly optimizations for performance")
set(ENABLE_EXTRATESTS OFF CACHE INTERNAL "Disable extra tests")
set(ENABLE_NC OFF CACHE INTERNAL "Disable netcat utility")

# We don't need libtls (high-level TLS API) - libcurl uses libssl directly
# But LibreSSL CMake doesn't have option to disable it, so we just don't link it

FetchContent_Declare(
    libressl
    URL https://cloudflare.cdn.openbsd.org/pub/OpenBSD/LibreSSL/libressl-4.2.1.tar.gz
    URL_HASH SHA256=6d5c2f58583588ea791f4c8645004071d00dfa554a5bf788a006ca1eb5abd70b
)

FetchContent_MakeAvailable(libressl)

# Apply Full LTO and -Oz to LibreSSL targets for maximum performance
if(TARGET crypto)
    target_compile_options(crypto PRIVATE -flto -Oz)
endif()
if(TARGET ssl)
    target_compile_options(ssl PRIVATE -flto -Oz)
endif()
if(TARGET tls)
    target_compile_options(tls PRIVATE -flto -Oz)
endif()

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
set(OPENSSL_VERSION "4.2.1" CACHE STRING "" FORCE)

# Make headers available
include_directories(SYSTEM "${libressl_SOURCE_DIR}/include")

message(STATUS "🔒 LibreSSL: Building from source with Full LTO -Oz (v4.2.1)")

# =============================================================================
# 3. Threads (this MUST come from OS - it's part of libc)
# =============================================================================
find_package(Threads REQUIRED)

# =============================================================================
# 4. CURL Configuration - ULTRA MINIMAL BUILD (HTTP/HTTPS only) with Full LTO
# =============================================================================
message(STATUS "📦 Configuring CURL (ultra minimal HTTP/HTTPS only)...")

# Build options
set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE INTERNAL "")

# SSL/TLS - Required for HTTPS
set(CURL_USE_OPENSSL ON CACHE INTERNAL "")

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

# CA bundle
set(CURL_CA_BUNDLE "auto" CACHE STRING "" FORCE)
set(CURL_CA_PATH "auto" CACHE STRING "" FORCE)

message(STATUS "📦 CURL: Ultra minimal build with Full LTO -Oz (HTTP/HTTPS only)")

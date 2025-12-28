# =============================================================================
# StaticDeps.cmake - Static Dependencies Configuration (ALL FROM SOURCE)
# =============================================================================
# Builds ALL dependencies from source for fully reproducible static builds.
# Only system dependency: libc (glibc/musl) and Perl (for OpenSSL build)
# =============================================================================

include(FetchContent)
include(ExternalProject)

# Detect number of CPUs for parallel build
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 1)
endif()

# =============================================================================
# 1. ZLIB - Build from source
# =============================================================================
message(STATUS "📦 Fetching zlib...")

set(ZLIB_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(SKIP_INSTALL_ALL ON CACHE INTERNAL "")

FetchContent_Declare(
    zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
)

FetchContent_MakeAvailable(zlib)

# Get the actual library path for zlibstatic
get_target_property(ZLIBSTATIC_LOCATION zlibstatic ARCHIVE_OUTPUT_DIRECTORY)
if(NOT ZLIBSTATIC_LOCATION)
    set(ZLIBSTATIC_LOCATION "${zlib_BINARY_DIR}")
endif()
set(ZLIB_LIBRARY_PATH "${ZLIBSTATIC_LOCATION}/libz.a")

# Create ZLIB::ZLIB as IMPORTED target for full CMake compatibility
# This is required for curl 8.17+ which does try_compile with ZLIB::ZLIB
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB STATIC IMPORTED GLOBAL)
    set_target_properties(ZLIB::ZLIB PROPERTIES
        IMPORTED_LOCATION "${ZLIB_LIBRARY_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}"
    )
    # Make ZLIB::ZLIB depend on zlibstatic so it gets built first
    add_dependencies(ZLIB::ZLIB zlibstatic)
endif()

# Set variables for other CMake scripts that use find_package(ZLIB)
set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
set(ZLIB_INCLUDE_DIRS "${ZLIB_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY "${ZLIB_LIBRARY_PATH}" CACHE STRING "" FORCE)
set(ZLIB_LIBRARIES "${ZLIB_LIBRARY_PATH}" CACHE STRING "" FORCE)

# Make zlib headers available globally
include_directories(SYSTEM ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

message(STATUS "🔒 ZLIB: Building from source (v1.3.1)")

# =============================================================================
# 2. OpenSSL - Build from source (superbuild pattern)
# =============================================================================
message(STATUS "📦 Building OpenSSL from source...")

set(OPENSSL_VERSION "3.6.0")
set(OPENSSL_INSTALL_DIR "${CMAKE_BINARY_DIR}/openssl-install")

# Download and build OpenSSL BEFORE configuring the rest
if(NOT EXISTS "${OPENSSL_INSTALL_DIR}/lib/libssl.a" OR NOT EXISTS "${OPENSSL_INSTALL_DIR}/include/openssl/ssl.h")
    message(STATUS "🔨 OpenSSL not found, building now (this may take a few minutes)...")
    
    # Download
    set(OPENSSL_TAR "${CMAKE_BINARY_DIR}/openssl-${OPENSSL_VERSION}.tar.gz")
    set(OPENSSL_SRC "${CMAKE_BINARY_DIR}/openssl-${OPENSSL_VERSION}")
    
    if(NOT EXISTS ${OPENSSL_TAR})
        message(STATUS "   Downloading OpenSSL ${OPENSSL_VERSION}...")
        file(DOWNLOAD 
            "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
            ${OPENSSL_TAR}
            SHOW_PROGRESS
            STATUS DOWNLOAD_STATUS
        )
        list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
        if(NOT STATUS_CODE EQUAL 0)
            message(FATAL_ERROR "Failed to download OpenSSL")
        endif()
    endif()
    
    # Extract
    if(NOT EXISTS ${OPENSSL_SRC})
        message(STATUS "   Extracting OpenSSL...")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf ${OPENSSL_TAR}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            RESULT_VARIABLE EXTRACT_RESULT
        )
        if(NOT EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to extract OpenSSL")
        endif()
    endif()
    
    # Configure (libraries only, no apps/tests/docs)
    # Note: OpenSSL LTO disabled - doesn't help with final binary size and adds complexity
    set(OPENSSL_CC "${CMAKE_C_COMPILER}")
    set(OPENSSL_AR "${CMAKE_AR}")
    set(OPENSSL_RANLIB "${CMAKE_RANLIB}")
    
    # Fallback to system tools if CMAKE_AR/RANLIB not set
    if(NOT OPENSSL_AR)
        set(OPENSSL_AR "ar")
    endif()
    if(NOT OPENSSL_RANLIB)
        set(OPENSSL_RANLIB "ranlib")
    endif()
    
    message(STATUS "   OpenSSL toolchain: CC=${OPENSSL_CC}, AR=${OPENSSL_AR}, RANLIB=${OPENSSL_RANLIB}")
    
    message(STATUS "   Configuring OpenSSL (libraries only, minimal ciphers)...")
    execute_process(
        COMMAND ./Configure 
            --prefix=${OPENSSL_INSTALL_DIR}
            --libdir=lib
            --release
            CC=${OPENSSL_CC}
            AR=${OPENSSL_AR}
            RANLIB=${OPENSSL_RANLIB}
            no-shared
            no-apps
            no-tests
            no-docs
            no-ui-console
            no-autoload-config
            # Disable legacy/weak ciphers (not needed for modern HTTPS)
            no-des
            no-rc2
            no-rc4
            no-rc5
            no-md4
            no-mdc2
            no-idea
            no-seed
            no-bf
            no-cast
            no-camellia
            no-aria
            no-sm2
            no-sm3
            no-sm4
            no-whirlpool
            no-rmd160
            no-scrypt
            no-ssl3
            no-dtls
            no-dtls1
            no-comp
            no-engine
            no-deprecated
            no-legacy
            # Keep only what we need for TLS 1.2/1.3
            linux-x86_64
        WORKING_DIRECTORY ${OPENSSL_SRC}
        RESULT_VARIABLE CONFIG_RESULT
        OUTPUT_QUIET
    )
    if(NOT CONFIG_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to configure OpenSSL")
    endif()
    
    # Build ONLY libraries (much faster than full build)
    message(STATUS "   Building OpenSSL libraries (using ${NPROC} cores)...")
    execute_process(
        COMMAND make -j${NPROC} build_libs 
            CC=${OPENSSL_CC}
            AR=${OPENSSL_AR}
            RANLIB=${OPENSSL_RANLIB}
        WORKING_DIRECTORY ${OPENSSL_SRC}
        RESULT_VARIABLE BUILD_RESULT
    )
    if(NOT BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to build OpenSSL libraries")
    endif()
    
    # Install libraries and headers only
    message(STATUS "   Installing OpenSSL libraries...")
    execute_process(
        COMMAND make install_dev
        WORKING_DIRECTORY ${OPENSSL_SRC}
        RESULT_VARIABLE INSTALL_RESULT
    )
    if(NOT INSTALL_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to install OpenSSL")
    endif()
    
    # Clean up source to save space (optional but recommended)
    message(STATUS "   Cleaning up OpenSSL source...")
    file(REMOVE_RECURSE ${OPENSSL_SRC})
    file(REMOVE ${OPENSSL_TAR})
endif()

# Now create imported targets pointing to our built OpenSSL
set(OPENSSL_INCLUDE_DIR "${OPENSSL_INSTALL_DIR}/include")

# Libraries are always in lib/ since we set --libdir=lib
set(OPENSSL_LIB_DIR "${OPENSSL_INSTALL_DIR}/lib")
if(NOT EXISTS "${OPENSSL_LIB_DIR}/libssl.a")
    message(FATAL_ERROR "OpenSSL build failed - libssl.a not found")
endif()

set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_LIB_DIR}/libcrypto.a")
set(OPENSSL_SSL_LIBRARY "${OPENSSL_LIB_DIR}/libssl.a")

add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
)

add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "OpenSSL::Crypto"
)

# Set CMake variables for compatibility
set(OPENSSL_FOUND TRUE CACHE BOOL "" FORCE)
set(OPENSSL_INCLUDE_DIR "${OPENSSL_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_CRYPTO_LIBRARY}" CACHE FILEPATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "${OPENSSL_SSL_LIBRARY}" CACHE FILEPATH "" FORCE)
set(OPENSSL_ROOT_DIR "${OPENSSL_INSTALL_DIR}" CACHE PATH "" FORCE)
set(OPENSSL_VERSION "${OPENSSL_VERSION}" CACHE STRING "" FORCE)

message(STATUS "🔒 OpenSSL: Built from source (v${OPENSSL_VERSION})")

# =============================================================================
# 3. Threads (this MUST come from OS - it's part of libc)
# =============================================================================
find_package(Threads REQUIRED)

# =============================================================================
# 4. CURL Configuration - MINIMAL BUILD (HTTP/HTTPS only)
# =============================================================================
# This project only needs HTTP/HTTPS for:
#   - Downloading speedtest CLI
#   - Fetching IP geolocation data
#   - Speed test server communication
#
# Disabled protocols/features (not needed):
#   - FTP, FTPS, SFTP, SCP (file transfer)
#   - GOPHER, GOPHERS (ancient protocol)
#   - IMAP, IMAPS, POP3, POP3S, SMTP, SMTPS (email)
#   - TELNET (remote shell)
#   - MQTT (IoT messaging)
#   - RTSP (streaming)
#   - DICT (dictionary)
#   - TFTP (trivial FTP)
#   - SMB, SMBS (Windows file sharing)
#   - LDAP, LDAPS (directory services)
# =============================================================================

# Build options
set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE INTERNAL "")

# SSL/TLS - Required for HTTPS
set(CURL_USE_OPENSSL ON CACHE INTERNAL "")

# Compression - Required for efficient transfers
set(CURL_ZLIB ON CACHE INTERNAL "")

# Disable optional features we don't need
set(CURL_USE_LIBPSL OFF CACHE INTERNAL "")
set(CURL_USE_LIBSSH2 OFF CACHE INTERNAL "")
set(CURL_USE_LIBSSH OFF CACHE INTERNAL "")
set(USE_LIBIDN2 OFF CACHE INTERNAL "")
set(CURL_USE_GSSAPI OFF CACHE INTERNAL "")
set(ENABLE_ARES OFF CACHE INTERNAL "")
set(CURL_BROTLI OFF CACHE INTERNAL "")
set(CURL_ZSTD OFF CACHE INTERNAL "")

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

# Keep HTTP/HTTPS enabled (default)
# CURL_DISABLE_HTTP is OFF by default
# CURL_DISABLE_HTTPS is OFF by default

# CA bundle - let curl find system CA at runtime
set(CURL_CA_BUNDLE "auto" CACHE STRING "" FORCE)
set(CURL_CA_PATH "auto" CACHE STRING "" FORCE)

message(STATUS "📦 CURL: Minimal build (HTTP/HTTPS only)")

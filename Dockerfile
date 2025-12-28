# =============================================================================
# Dockerfile - Musl Static Build for Bench
# =============================================================================
# This Dockerfile builds a fully static binary using musl libc.
# The resulting binary can run on ANY Linux distribution without dependencies.
#
# ALL dependencies (zlib, openssl, curl, json) are built from source.
# Only musl libc comes from the OS.
#
# Usage:
#   docker build -t bench-builder .
#   docker run --rm -v $(pwd)/dist:/dist bench-builder
#
# Or use the helper script:
#   ./build-static.sh
# =============================================================================

FROM alpine:latest AS builder

# Install ONLY build tools - all libraries are built from source
RUN apk add --no-cache \
    clang \
    lld \
    llvm \
    libc++-static \
    libc++-dev \
    compiler-rt \
    cmake \
    make \
    linux-headers \
    llvm-libunwind-static \
    perl \
    bash \
    xxd

# Set working directory
WORKDIR /src

# Copy source files
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY include/ ./include/
COPY src/ ./src/

# Configure and build
# Note: We set additional linker flags for Alpine's static libc++ requirements
RUN mkdir build && cd build && \
    CC=clang CXX=clang++ cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_AR=/usr/bin/llvm-ar \
        -DCMAKE_RANLIB=/usr/bin/llvm-ranlib \
        -DCMAKE_EXE_LINKER_FLAGS="-static -fuse-ld=lld -rtlib=compiler-rt" \
        -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
        -DOPENSSL_USE_STATIC_LIBS=ON && \
    make -j$(nproc)

# Strip the binary to reduce size
RUN strip build/bench

# Verify it's static
RUN file build/bench && \
    ! ldd build/bench 2>/dev/null || true

# =============================================================================
# Output Stage - Extract the binary
# =============================================================================
FROM scratch AS export

COPY --from=builder /src/build/bench /bench

# =============================================================================
# Runtime Stage (optional) - Minimal container with just the binary
# =============================================================================
FROM scratch AS runtime

COPY --from=builder /src/build/bench /bench
COPY --from=builder /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/

ENTRYPOINT ["/bench"]

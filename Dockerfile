# =============================================================================
# Dockerfile - Musl Static Build for Bench
# =============================================================================
# Optimized for: Maximum performance + Security hardening
# =============================================================================
FROM alpine:latest AS builder

# 1. Install Build Tools (minimal required packages)
# Note: perl/bash NOT needed - LibreSSL uses CMake native
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
    xxd

# Set working directory
WORKDIR /src

# Copy source files
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY include/ ./include/
COPY src/ ./src/

# 2. Configure and build with full optimizations
# - Full LLVM stack (clang + lld + llvm-ar)
# - Full LTO for maximum optimization
# - Security hardening flags applied via CMakeLists.txt
RUN mkdir build && cd build && \
    CC=clang CXX=clang++ cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_AR=/usr/bin/llvm-ar \
        -DCMAKE_RANLIB=/usr/bin/llvm-ranlib \
        -DCMAKE_EXE_LINKER_FLAGS="-static -fuse-ld=lld -rtlib=compiler-rt" \
        -DCMAKE_CXX_FLAGS="-stdlib=libc++" && \
    make -j$(nproc)

# Strip binary (remove debug symbols for smaller size)
RUN strip build/bench

# =============================================================================
# Runtime Stage - PURE SCRATCH (Kosong melompong)
# =============================================================================
FROM scratch AS runtime

# Copy binary doang.
# GAK PERLU copy ca-certificates.crt lagi (karena udah embedded)
COPY --from=builder /src/build/bench /bench

ENTRYPOINT ["/bench"]
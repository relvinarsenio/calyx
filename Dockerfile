# =============================================================================
# Dockerfile - Musl Static Build for Calyx
# =============================================================================
# Optimized for: Maximum performance + Security hardening
# =============================================================================
FROM alpine:latest AS builder

# 1. Install Build Tools (minimal required packages)
# Note: perl is required for some OpenSSL/LibreSSL build scripts
RUN apk add --no-cache \
    clang \
    lld \
    llvm \
    libc++-static \
    libc++-dev \
    compiler-rt \
    cmake \
    ninja \
    linux-headers \
    llvm-libunwind-static \
    perl \
    xxd \
    liburing-dev

# Set working directory
WORKDIR /src

# Copy source files
COPY CMakeLists.txt ./
COPY LICENSE ./
COPY cmake/ ./cmake/
COPY include/ ./include/
COPY src/ ./src/

# 2. Configure and build with full optimizations
# - Full LLVM stack (clang + lld + llvm-ar)
# - Full LTO for maximum optimization
# - Security hardening flags applied via CMakeLists.txt
# 2. Configure and build with full optimizations
RUN CC=clang CXX=clang++ cmake -B build -S . \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_AR=/usr/bin/llvm-ar \
        -DCMAKE_RANLIB=/usr/bin/llvm-ranlib \
        -DCMAKE_EXE_LINKER_FLAGS="-static -fuse-ld=lld -rtlib=compiler-rt" \
        -DCMAKE_CXX_FLAGS="-stdlib=libc++" && \
    cmake --build build --parallel $(nproc)

# Strip binary (remove debug symbols for smaller size)
RUN strip build/calyx

# =============================================================================
# Runtime Stage - PURE SCRATCH (Kosong melompong)
# =============================================================================
FROM scratch AS runtime

# Copy binary only.
# No need to copy ca-certificates.crt again (already embedded)
COPY --from=builder /src/build/calyx /calyx

ENTRYPOINT ["/calyx"]
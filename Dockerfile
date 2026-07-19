# =============================================================================
# Dockerfile - Musl Static Build Toolchain for Calyx
# =============================================================================
FROM alpine:latest

# Docker Buildx provides TARGETARCH automatically
ARG TARGETARCH

# Note: perl is required for some OpenSSL/LibreSSL build scripts
# Separate cache by architecture to prevent cache corruption during cross-compilation
RUN --mount=type=cache,id=apk-${TARGETARCH},target=/var/cache/apk \
    apk add \
    ccache \
    clang \
    cmake \
    coreutils \
    g++ \
    gcc \
    libstdc++ \
    libstdc++-dev \
    liburing-dev \
    linux-headers \
    lld \
    llvm \
    musl-dev \
    nghttp2-dev \
    nghttp2-static \
    ninja \
    perl \
    && mkdir -p /src /build /root/.ccache

WORKDIR /src

# Environment Configuration
ENV CCACHE_DIR=/root/.ccache
ENV CCACHE_MAXSIZE=500M
ENV CMAKE_GENERATOR=Ninja
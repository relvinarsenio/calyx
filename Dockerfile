# =============================================================================
# Dockerfile - Musl Static Build Toolchain for Calyx
# =============================================================================
FROM alpine:latest

# Note: perl is required for some OpenSSL/LibreSSL build scripts
RUN --mount=type=cache,target=/var/cache/apk \
    apk add \
    ccache \
    clang \
    cmake \
    gcc \
    libstdc++ \
    libstdc++-dev \
    liburing-dev \
    linux-headers \
    lld \
    llvm \
    ninja \
    perl \
    xxd \
    && mkdir -p /src /build /root/.ccache

WORKDIR /src

# Environment Configuration
ENV CCACHE_DIR=/root/.ccache
ENV CCACHE_MAXSIZE=500M
ENV CMAKE_GENERATOR=Ninja
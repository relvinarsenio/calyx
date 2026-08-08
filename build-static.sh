#!/bin/bash
# =============================================================================
# build-static.sh - Build fully static musl binary using Docker
# =============================================================================
# Strategy: Build toolchain image ONCE, compile via container with
# persistent build/ccache volumes. Source is bind-mounted directly
# from the host for zero-overhead, real-time file access.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# =============================================================================
# 0. Configuration
# =============================================================================
PROJECT_NAME="calyx"
BINARY_NAME="${PROJECT_NAME}"
DIST_DIR="dist"

IMAGE_NAME="${PROJECT_NAME}-builder"
CONTAINER_NAME="${PROJECT_NAME}-compile"
VOL_BUILD="${PROJECT_NAME}-build-cache"
VOL_CCACHE="${PROJECT_NAME}-ccache"

# Internal container paths
C_SRC="/src"
C_BUILD="/build"
C_CCACHE="/root/.ccache"

# Compiler settings
CC="clang"
CXX="clang++"
STRIP="llvm-strip"
AR="llvm-ar"
RANLIB="llvm-ranlib"
LINKER="LLD"
BUILD_TYPE="MinSizeRel"

PLATFORM=""

# =============================================================================
# 1. Trap / Auto-Cleanup on Interrupt
# =============================================================================
cleanup_on_interrupt() {
    echo ""
    echo "🛑  Build cancelled by user (Ctrl+C)!"
    echo "🧹  Cleaning up..."

    docker container stop -t 0 "$CONTAINER_NAME" >/dev/null 2>&1 || true
    docker container rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true

    echo "✨  Cleanup complete."
    exit 1
}

trap cleanup_on_interrupt SIGINT SIGTERM

clean_docker_assets() {
    # Graceful stop
    docker ps -a --format '{{.Names}}' | grep "^${PROJECT_NAME}-" | xargs -r docker stop -t 5 >/dev/null 2>&1 || true
    # Remove containers
    docker ps -a --format '{{.Names}}' | grep "^${PROJECT_NAME}-" | xargs -r docker rm -f >/dev/null 2>&1 || true
    # Remove volumes
    docker volume ls --format '{{.Name}}' | grep "^${PROJECT_NAME}-" | xargs -r docker volume rm >/dev/null 2>&1 || true
    # Remove images
    docker image ls --format '{{.Repository}}:{{.Tag}}' | grep -E "(/|^)${PROJECT_NAME}-" | xargs -r docker image rm -f >/dev/null 2>&1 || true
    # Prune any dangling images left behind
    docker image prune -f >/dev/null 2>&1 || true
}

# =============================================================================
# 2. Parse Arguments
# =============================================================================
FRESH_BUILD=false
REBUILD_IMAGE=false
UPDATE_IMAGE=false
CLEAN_ALL=false

show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Build a fully static musl binary using Docker."
    echo ""
    echo "Options:"
    echo "  -c, --clean            Deep clean all Docker assets related to this project"
    echo "  -f, --fresh-build      Clear build cache only (forces CMake re-config)"
    echo "  -r, --rebuild-image    Rebuild toolchain image and clear ALL caches"
    echo "  -u, --update-image     Pull the latest builder image from registry before compiling"
    echo "  -p, --platform <arch>  Docker platform (e.g. linux/arm64, linux/amd64)"
    echo "  -h, --help             Show this help message"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--clean)
            CLEAN_ALL=true
            shift
            ;;
        -f|--fresh-build)
            FRESH_BUILD=true
            shift
            ;;
        -r|--rebuild-image)
            REBUILD_IMAGE=true
            shift
            ;;
        -u|--update-image)
            UPDATE_IMAGE=true
            shift
            ;;
        -p|--platform)
            if [[ -z "${2:-}" ]]; then
                echo "❌ Error: -p or --platform requires an architecture argument (e.g., linux/amd64)."
                exit 1
            fi
            PLATFORM="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

if [ "$CLEAN_ALL" = "true" ]; then
    echo "🧹 Deep cleaning all ${PROJECT_NAME}-related Docker assets..."
    clean_docker_assets
    echo "✨ Deep cleanup complete. No other Docker projects were affected."
    exit 0
fi

# Suffix container/volume names with platform arch to avoid clashes
if [[ -n "$PLATFORM" ]]; then
    # 1. Check if platform is supported by docker buildx
    if ! docker buildx ls | grep -q "$PLATFORM"; then
        echo "❌ Error: Platform '$PLATFORM' is not supported by your Docker installation."
        echo "   Check 'docker buildx ls' for available platforms."
        exit 1
    fi

    # 2. Check for binfmt_misc if we are cross-compiling (not native platform)
    HOST_ARCH=$(uname -m)
    case "$PLATFORM" in
        *arm64*|*aarch64*) TARGET_ARCH="aarch64" ;;
        *amd64*|*x86_64*)  TARGET_ARCH="x86_64" ;;
        *) TARGET_ARCH="unknown" ;;
    esac

    if [[ "$TARGET_ARCH" != "unknown" && "$TARGET_ARCH" != "$HOST_ARCH" ]]; then
        if ! grep -q "enabled" /proc/sys/fs/binfmt_misc/status 2>/dev/null; then
            echo "⚠️  Warning: Cross-platform build requested but binfmt_misc is not enabled."
            echo "   Emulated execution (via QEMU) might fail with 'exec format error'."
            echo "   Try: docker run --privileged --rm tonistiigi/binfmt --install all"
            echo ""
        fi
    fi

    ARCH_SUFFIX="-$(echo "$PLATFORM" | tr '/' '-')"
    IMAGE_NAME="${IMAGE_NAME}${ARCH_SUFFIX}"
    CONTAINER_NAME="${CONTAINER_NAME}${ARCH_SUFFIX}"
    VOL_BUILD="${VOL_BUILD}${ARCH_SUFFIX}"
    VOL_CCACHE="${VOL_CCACHE}${ARCH_SUFFIX}"
fi

# =============================================================================
# 3. Build Toolchain Image (only if needed)
# =============================================================================
NEED_BUILD=false
GHCR_IMAGE="${GHCR_IMAGE:-ghcr.io/relvinarsenio/calyx-builder}"

case "$PLATFORM" in
    *arm64*|*aarch64*) GHCR_TAG="arm64" ;;
    *amd64*|*x86_64*)  GHCR_TAG="amd64" ;;
    *) 
        if [[ "$(uname -m)" == "aarch64" ]]; then GHCR_TAG="arm64"; else GHCR_TAG="amd64"; fi
        ;;
esac

if [[ "$REBUILD_IMAGE" == "true" ]]; then
    echo "🗑️  Rebuild image requested — rebuilding toolchain and clearing everything..."
    clean_docker_assets
    NEED_BUILD=true
elif [[ "$UPDATE_IMAGE" == "true" ]] || ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
    if [[ "$UPDATE_IMAGE" == "true" ]]; then
        echo "🔄 Update image requested — pulling latest toolchain from GHCR ($GHCR_IMAGE:${GHCR_TAG})..."
    else
        echo "📦 Toolchain image not found locally."
        echo "⬇️  Attempting to pull pre-built image from GHCR ($GHCR_IMAGE:${GHCR_TAG})..."
    fi
    if docker pull "$GHCR_IMAGE:${GHCR_TAG}"; then
        echo "✅ Successfully pulled pre-built image!"
        docker tag "$GHCR_IMAGE:${GHCR_TAG}" "$IMAGE_NAME"
        docker rm -f "$CONTAINER_NAME" &>/dev/null || true
        NEED_BUILD=false
    else
        echo "⚠️  Pull failed or image not available. Building toolchain from scratch..."
        NEED_BUILD=true
    fi
else
    if docker container inspect "$CONTAINER_NAME" &>/dev/null; then
        if [[ $(docker container inspect -f "{{ range .Mounts }}{{ if eq .Destination \"$C_SRC\" }}{{ .Source }}{{ break }}{{ end }}{{ end }}" "$CONTAINER_NAME" 2>/dev/null || true) != "$SCRIPT_DIR" ]]; then
            NEED_BUILD=true
        fi
    fi
fi

if [[ "$NEED_BUILD" == "true" ]]; then
    OLD_IMAGE_ID="$(docker image ls -q "$IMAGE_NAME" 2>/dev/null)"

    DOCKER_ARGS=(--force-rm)
    if [[ "$REBUILD_IMAGE" == "true" ]]; then
        DOCKER_ARGS+=(--no-cache)
    fi
    if [[ -n "$PLATFORM" ]]; then
        DOCKER_ARGS+=(--platform "$PLATFORM")
    fi

    echo ""
    echo "🐳 Building toolchain image (Alpine/musl)..."
    echo "=============================================="
    DOCKER_BUILDKIT=1 docker image build "${DOCKER_ARGS[@]}" -t "$IMAGE_NAME" .

    if [[ -n "$OLD_IMAGE_ID" ]]; then
        NEW_IMAGE_ID="$(docker image ls -q "$IMAGE_NAME" 2>/dev/null)"
        if [[ "$OLD_IMAGE_ID" != "$NEW_IMAGE_ID" ]]; then
            echo ""
            echo "🧹 Removing old toolchain image ($OLD_IMAGE_ID)..."
            docker image rm "$OLD_IMAGE_ID" >/dev/null 2>&1 || true
        fi
    fi
fi

# =============================================================================
# 4. Start / Reuse Build Container
# =============================================================================
echo ""
echo "🔨 Compiling static binary..."
echo "=============================================="

mkdir -p "$SCRIPT_DIR/$DIST_DIR"

# REUSE: Create container only if it doesn't exist
if [[ "$NEED_BUILD" == "true" ]]; then
    docker container rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
fi

if ! docker container inspect "$CONTAINER_NAME" &>/dev/null; then
    DOCKER_CREATE_ARGS=(--name "$CONTAINER_NAME" -v "$SCRIPT_DIR:$C_SRC:Z" -v "$VOL_BUILD:$C_BUILD" -v "$VOL_CCACHE:$C_CCACHE" -i)
    [[ -n "$PLATFORM" ]] && DOCKER_CREATE_ARGS+=(--platform "$PLATFORM")
    docker container create "${DOCKER_CREATE_ARGS[@]}" "$IMAGE_NAME" sh >/dev/null
fi

if [[ "$(docker container inspect -f '{{.State.Running}}' "$CONTAINER_NAME" 2>/dev/null)" != "true" ]]; then
    docker container start "$CONTAINER_NAME" >/dev/null
fi

# =============================================================================
# 5. Build Inside Container
# =============================================================================
docker container exec \
    -e FRESH_BUILD="$FRESH_BUILD" \
    -e C_BUILD="$C_BUILD" \
    -e C_SRC="$C_SRC" \
    -e BUILD_TYPE="$BUILD_TYPE" \
    -e CXX="$CXX" \
    -e CC="$CC" \
    -e AR="$AR" \
    -e RANLIB="$RANLIB" \
    -e LINKER="$LINKER" \
    -e BINARY_NAME="$BINARY_NAME" \
    -e STRIP="$STRIP" \
    "$CONTAINER_NAME" sh -c '
    if [ "$FRESH_BUILD" = "true" ]; then
        if [ -n "$C_BUILD" ] && [ "$C_BUILD" != "/" ]; then
            find "$C_BUILD" -mindepth 1 -delete
        fi
    fi

    # Sync bind-mount metadata to ensure host changes are visible
    find "$C_SRC" -maxdepth 2 > /dev/null
    sync

    if [ ! -f "$C_BUILD/CMakeCache.txt" ]; then
        cmake -B "$C_BUILD" -S "$C_SRC" \
            -DCMAKE_POLICY_DEFAULT_CMP0149=NEW \
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
            -DCMAKE_CXX_COMPILER="$CXX" \
            -DCMAKE_C_COMPILER="$CC" \
            -DCMAKE_AR="$AR" \
            -DCMAKE_RANLIB="$RANLIB" \
            -DCMAKE_LINKER_TYPE="$LINKER"
    fi

    cmake --build "$C_BUILD" --parallel $(nproc) && \
    "$STRIP" "$C_BUILD/$BINARY_NAME"
'

# =============================================================================
# 6. Extract Binary & Stop Container
# =============================================================================
echo ""
echo "📦 Extracting binary..."

docker container cp "$CONTAINER_NAME":$C_BUILD/${BINARY_NAME} "$SCRIPT_DIR/$DIST_DIR/${BINARY_NAME}" || {
    echo "❌ Failed to extract binary! Was it compiled successfully?"
    docker container stop -t 0 "$CONTAINER_NAME" >/dev/null 2>&1 || true
    exit 1
}
docker container stop -t 0 "$CONTAINER_NAME" >/dev/null 2>&1 || true


# =============================================================================
# 7. Results
# =============================================================================
echo ""
echo "✅ Build complete!"
echo "=============================================="
file "$SCRIPT_DIR/$DIST_DIR/${BINARY_NAME}"
ls -lh "$SCRIPT_DIR/$DIST_DIR/${BINARY_NAME}"
echo ""
echo "Binary location: ./$DIST_DIR/${BINARY_NAME}"
echo ""
echo "Test with: ./$DIST_DIR/${BINARY_NAME}"


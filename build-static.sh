#!/bin/bash
# =============================================================================
# build-static.sh - Build fully static musl binary using Docker
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

IMAGE_NAME="bench-builder"

# =============================================================================
# 0. Trap / Auto-Cleanup on Interrupt
# =============================================================================
cleanup_on_interrupt() {
    echo ""
    echo "🛑  Build cancelled by user (Ctrl+C)!"
    echo "🧹  Cleaning up build containers..."
    
    docker rm -f bench-extract >/dev/null 2>&1 || true
    

    docker rm -f $(docker ps -a -q --filter "ancestor=$IMAGE_NAME") >/dev/null 2>&1 || true

    echo "⚠️  Note: Dangling images (<none>:<none>) might remain."
    echo "    Run 'docker image prune' manually if needed."
    echo "✨  Cleanup complete."
    exit 1
}

trap cleanup_on_interrupt SIGINT SIGTERM

# =============================================================================
# 1. Parse Arguments
# =============================================================================
FRESH_BUILD=false

show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Build a fully static musl binary using Docker."
    echo ""
    echo "Options:"
    echo "  --fresh-build    Force rebuild from scratch (no cache)"
    echo "  -h, --help       Show this help message"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --fresh-build)
            FRESH_BUILD=true
            shift
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

# =============================================================================
# 2. Build Configuration
# =============================================================================
DOCKER_ARGS_BASE="--force-rm"
DOCKER_ARGS=""
OLD_IMAGE_ID=""

if [ "$FRESH_BUILD" = true ]; then
    echo "🗑️  Fresh build requested (No Cache Mode)..."
    OLD_IMAGE_ID="$(docker images -q "$IMAGE_NAME" 2>/dev/null)"
    DOCKER_ARGS="--no-cache"
    echo ""
fi

echo "🐳 Building static binary with Docker (Alpine/musl)..."
echo "=============================================="

# Build using Docker
docker build $DOCKER_ARGS_BASE $DOCKER_ARGS --target builder -t "$IMAGE_NAME" .

# =============================================================================
# 3. Cleanup Phase (On Success)
# =============================================================================
if [ "$FRESH_BUILD" = true ] && [ -n "$OLD_IMAGE_ID" ]; then
    NEW_IMAGE_ID="$(docker images -q "$IMAGE_NAME" 2>/dev/null)"
    if [ "$OLD_IMAGE_ID" != "$NEW_IMAGE_ID" ]; then
        echo ""
        echo "🧹 Cleaning up old dangling image ($OLD_IMAGE_ID)..."
        docker rmi "$OLD_IMAGE_ID" >/dev/null 2>&1 || true
        echo "✨ Old image removed."
    fi
fi

# =============================================================================
# 4. Extraction Phase
# =============================================================================
echo ""
echo "📦 Extracting binary..."

# FIX: Create directory RIGHT HERE to guarantee it exists
mkdir -p "$SCRIPT_DIR/dist"

docker rm -f bench-extract 2>/dev/null || true
docker create --name bench-extract "$IMAGE_NAME" /bin/true

# Copy to explicit path
docker cp bench-extract:/src/build/bench "$SCRIPT_DIR/dist/bench"

docker rm bench-extract

# Show results
echo ""
echo "✅ Build complete!"
echo "=============================================="
file "$SCRIPT_DIR/dist/bench"
ls -lh "$SCRIPT_DIR/dist/bench"
echo ""
echo "Binary location: ./dist/bench"
echo ""
echo "Test with: ./dist/bench"

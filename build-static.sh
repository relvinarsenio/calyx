#!/bin/bash
# =============================================================================
# build-static.sh - Build fully static musl binary using Docker
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

IMAGE_NAME="bench-builder"

# =============================================================================
# Parse Arguments
# =============================================================================
FRESH_BUILD=false

show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Build a fully static musl binary using Docker."
    echo ""
    echo "Options:"
    echo "  --fresh-build    Remove existing Docker image and rebuild from scratch"
    echo "  -h, --help       Show this help message"
    echo ""
    echo "Example:"
    echo "  $0               # Normal build (uses cached image if available)"
    echo "  $0 --fresh-build # Clean build (removes existing image first)"
}

for arg in "$@"; do
    case $arg in
        --fresh-build)
            FRESH_BUILD=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            show_help
            exit 1
            ;;
    esac
done

# =============================================================================
# Fresh Build: Remove existing image
# =============================================================================
if [ "$FRESH_BUILD" = true ]; then
    echo "🗑️  Fresh build requested, removing existing Docker image..."
    docker rmi -f "$IMAGE_NAME" 2>/dev/null || true
    echo ""
fi

echo "🐳 Building static binary with Docker (Alpine/musl)..."
echo "=============================================="

# Create dist directory
mkdir -p dist

# Build using Docker (target builder stage only)
docker build --target builder -t "$IMAGE_NAME" .

# Extract the binary
echo ""
echo "📦 Extracting binary..."
docker rm -f bench-extract 2>/dev/null || true
docker create --name bench-extract "$IMAGE_NAME" /bin/true
docker cp bench-extract:/src/build/bench ./dist/bench
docker rm bench-extract

# Show results
echo ""
echo "✅ Build complete!"
echo "=============================================="
file ./dist/bench
ls -lh ./dist/bench
echo ""
echo "Binary location: ./dist/bench"
echo ""
echo "Test with: ./dist/bench"

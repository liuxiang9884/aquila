#!/bin/bash

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$SCRIPT_DIR/build"

# Default number of parallel compilation cores
DEFAULT_JOBS=8
JOBS=$DEFAULT_JOBS

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -n|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [options] [build_type]"
            echo ""
            echo "Build types:"
            echo "  debug               Build Debug version"
            echo "  release             Build Release version"
            echo "  release_with_debug  Build Release version with debug info"
            echo "  release_asan        Build Release version with ASAN"
            echo ""
            echo "Options:"
            echo "  -n, --jobs N        Set number of parallel compilation cores (default: $DEFAULT_JOBS)"
            echo "  -h, --help          Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0 release          Build Release version with default 8 cores"
            echo "  $0 -n 4 debug       Build Debug version with 4 cores"
            echo "  $0 --jobs 16 release  Build Release version with 16 cores"
            exit 0
            ;;
        debug|release|release_with_debug|release_asan)
            BUILD_TYPE="$1"
            shift
            ;;
        *)
            echo "Error: Unknown parameter '$1'"
            echo "Use '$0 --help' to see help information"
            exit 1
            ;;
    esac
done

# Validate core count parameter
if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [ "$JOBS" -lt 1 ]; then
    echo "Error: Number of cores must be a positive integer"
    exit 1
fi

echo "Using $JOBS cores for parallel compilation"

mkdir -p "$BUILD_DIR"

build_debug() {
    echo "Building DEBUG version..."
    mkdir -p "$BUILD_DIR/debug"
    cd "$BUILD_DIR/debug" || exit 1
    cmake -DCMAKE_BUILD_TYPE=Debug "$SCRIPT_DIR" || exit 1
    cmake --build . --parallel "$JOBS"
}

build_release() {
    echo "Building RELEASE version (optimized)..."
    mkdir -p "$BUILD_DIR/release"
    cd "$BUILD_DIR/release" || exit 1
    cmake -DCMAKE_BUILD_TYPE=Release "$SCRIPT_DIR" || exit 1
    cmake --build . --parallel "$JOBS"
}

build_release_with_debug() {
    echo "Building RELEASE version with GDB support (-O1 -g)..."
    mkdir -p "$BUILD_DIR/release_with_debug"
    cd "$BUILD_DIR/release_with_debug" || exit 1
    cmake \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_FLAGS="-O1 -g" \
        -DCMAKE_C_FLAGS="-O1 -g" \
        "$SCRIPT_DIR" || exit 1  # Explicitly pass source directory path
    cmake --build . --parallel "$JOBS"
}

build_release_asan() {
    echo "Building RELEASE version with ASAN (-O1 -g -fsanitize=address)..."
    mkdir -p "$BUILD_DIR/release_asan"
    cd "$BUILD_DIR/release_asan" || exit 1
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-O1 -g -fsanitize=address" \
        -DCMAKE_C_FLAGS="-O1 -g -fsanitize=address" \
        "$SCRIPT_DIR" || exit 1
    cmake --build . --parallel "$JOBS"
}

case "$BUILD_TYPE" in
    debug) build_debug ;;
    release) build_release ;;
    release_with_debug) build_release_with_debug ;;
    release_asan) build_release_asan ;;
    *)
        # Default build Debug and Release when no arguments provided
        build_debug
        build_release
        ;;
esac

if [ $? -eq 0 ]; then
    echo "Build completed successfully!"
else
    echo "Build failed! Check error messages above."
    exit 1
fi

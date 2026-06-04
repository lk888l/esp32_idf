#!/bin/bash
# build.sh - Build script for ESP-IDF project

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
IDF_TARGET="${IDF_TARGET:-esp32}"

echo "=========================================="
echo "ESP-IDF Project Builder"
echo "=========================================="
echo "Project: sample_project"
echo "Target: $IDF_TARGET"
echo "Build Dir: $BUILD_DIR"
echo "=========================================="

# Check if IDF_PATH is set
if [ -z "$IDF_PATH" ]; then
    echo "Error: IDF_PATH is not set"
    echo "Please run: export IDF_PATH=<path-to-esp-idf>"
    exit 1
fi

# Check if idf.py exists
if ! command -v idf.py &> /dev/null; then
    export PATH="$IDF_PATH/tools:$PATH"
fi

# Clean build if requested
if [ "$1" = "clean" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    exit 0
fi

# Build the project
echo "Building project..."
cd "$PROJECT_DIR"
idf.py -B "$BUILD_DIR" build

# Display build summary
if [ -d "$BUILD_DIR" ]; then
    echo ""
    echo "=========================================="
    echo "Build completed successfully!"
    echo "=========================================="
    
    # Show binary info
    if [ -f "$BUILD_DIR/sample_project.bin" ]; then
        SIZE=$(stat -f "$BUILD_DIR/sample_project.bin" 2>/dev/null | grep -o 'Size:[^[:space:]]*' || echo "Size: N/A")
        echo "Binary: $BUILD_DIR/sample_project.bin ($SIZE)"
    fi
fi

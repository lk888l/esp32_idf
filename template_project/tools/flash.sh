#!/bin/bash
# flash.sh - Flash script for ESP-IDF project

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
PORT="${1:-/dev/ttyUSB0}"

# Check if device is connected
if [ ! -e "$PORT" ]; then
    echo "Error: Device not found at $PORT"
    echo "Available ports:"
    ls -la /dev/ttyUSB* 2>/dev/null || echo "  No devices found"
    exit 1
fi

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory not found at $BUILD_DIR"
    echo "Please run: ./tools/build.sh"
    exit 1
fi

echo "=========================================="
echo "Flashing ESP-IDF Project"
echo "=========================================="
echo "Port: $PORT"
echo "Build: $BUILD_DIR"
echo "=========================================="

# Flash the project
idf.py -p "$PORT" -B "$BUILD_DIR" flash

echo ""
echo "=========================================="
echo "Flashing completed!"
echo "=========================================="
echo "To monitor output, run:"
echo "  idf.py -p $PORT monitor"

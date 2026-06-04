#!/bin/bash
# monitor.sh - Serial monitor script

PORT="${1:-/dev/ttyUSB0}"
BAUD="${2:-115200}"

if [ ! -e "$PORT" ]; then
    echo "Error: Device not found at $PORT"
    exit 1
fi

echo "Starting serial monitor on $PORT at $BAUD baud..."
idf.py -p "$PORT" monitor -b "$BAUD"

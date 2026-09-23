#!/usr/bin/env bash
# Run after sourcing ESP-IDF. Host tests and build only; no RF or flashing.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
command -v idf.py >/dev/null || { echo 'Load ESP-IDF first: source /opt/esp-idf/export.sh' >&2; exit 2; }
cmake -S tests -B build/gateway-host-tests -G Ninja -DM5_HOST_SANITIZERS=ON
cmake --build build/gateway-host-tests
ctest --test-dir build/gateway-host-tests --output-on-failure
# Generated SDKCONFIG isolates verification from the user's normal config.
cp sdkconfig build/gateway-verification.sdkconfig
idf.py -B build/gateway-verification -D SDKCONFIG="$project_dir/build/gateway-verification.sdkconfig" build

#!/usr/bin/env bash
# Run inside the configured ESP-IDF Docker container; never flash hardware.
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
if ! command -v idf.py >/dev/null 2>&1; then
    printf '%s\n' 'Load ESP-IDF first: source /opt/esp/idf/export.sh' >&2
    exit 2
fi

cmake -S tests -B build/peripheral-host-tests -G Ninja
cmake --build build/peripheral-host-tests
ctest --test-dir build/peripheral-host-tests --output-on-failure
cmake -S tests -B build/peripheral-sanitizers -G Ninja -DM5_HOST_SANITIZERS=ON
cmake --build build/peripheral-sanitizers
ctest --test-dir build/peripheral-sanitizers --output-on-failure

cp sdkconfig build/peripherals.sdkconfig
idf.py -B build/peripherals-idf -D SDKCONFIG="$project_dir/build/peripherals.sdkconfig" build

# Exercise the actual LVGL pages with deterministic service snapshots off-device.
cmake -S tests/ui -B build/ui-preview -G Ninja
cmake --build build/ui-preview
ctest --test-dir build/ui-preview --output-on-failure

#!/usr/bin/env bash
# Run in the existing ESP-IDF container. Builds and host tests only; no flashing.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
command -v idf.py >/dev/null || { echo 'Load ESP-IDF: source /opt/esp-idf/export.sh' >&2; exit 2; }
cmake -S tests -B build/dap-host-tests -G Ninja -DM5_HOST_SANITIZERS=ON
cmake --build build/dap-host-tests
ctest --test-dir build/dap-host-tests --output-on-failure
cmake -S tests/ui -B build/dap-ui-tests -G Ninja
cmake --build build/dap-ui-tests
ctest --test-dir build/dap-ui-tests --output-on-failure
cp sdkconfig build/dap-verification.sdkconfig
idf.py -B build/dap-verification -D SDKCONFIG="$project_dir/build/dap-verification.sdkconfig" build
for mode in usb wifi ble; do
    openocd -f "tools/openocd/sticks3-$mode.cfg" -c shutdown
done

#!/usr/bin/env bash
set -euo pipefail

# esp-dev intentionally starts with a minimal PATH.  Load the tool paths in
# this process only, then replace the shell with idf.py so signals propagate.
idf_root="${IDF_PATH:-/opt/esp/idf}"
# shellcheck disable=SC1091
source "${idf_root}/export.sh" >/dev/null
exec idf.py "$@"

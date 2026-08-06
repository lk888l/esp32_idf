#!/usr/bin/env bash
set -euo pipefail

# esp-dev intentionally starts with a minimal PATH. Load the tool paths in
# this process only, then replace the shell with idf.py so signals propagate.
idf_root="${IDF_PATH:-/opt/esp/idf}"
# shellcheck disable=SC1091
source "${idf_root}/export.sh" >/dev/null

# Wireless credentials never belong in the tracked sdkconfig. If a protected
# overlay exists, build against an ignored local sdkconfig instead.
if [[ -f sdkconfig.secrets ]]; then
    export SDKCONFIG="${SDKCONFIG:-sdkconfig.local}"
    export SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS:-sdkconfig.defaults};sdkconfig.secrets"
fi

exec idf.py "$@"

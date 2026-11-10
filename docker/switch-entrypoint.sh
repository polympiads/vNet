#!/bin/bash
set -e

# Without VNET_CONFIG_PATH the switch gets its config from the conductor.
if [ -n "${VNET_CONFIG_PATH:-}" ] && [ ! -f "$VNET_CONFIG_PATH" ]; then
    echo "[Switch] ERROR: config file not found or is a directory: $VNET_CONFIG_PATH"
    exit 1
fi

exec "$@"

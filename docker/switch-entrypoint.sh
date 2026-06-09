#!/bin/bash
set -e

if [ ! -f "${VNET_CONFIG_PATH:-/etc/vnet/config.txt}" ]; then
    echo "[Switch] ERROR: config file not found or is a directory: ${VNET_CONFIG_PATH:-/etc/vnet/config.txt}"
    exit 1
fi

exec "$@"

#!/bin/bash
# Certificates for docker-compose.yml: a CA and one certificate per service,
# in docker/certs (ignored by git). Run once before docker compose up.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CERTS="$ROOT/docker/certs"
[ -f "$CERTS/ca.pem" ] || "$ROOT/tools/vnet-certs.sh" init "$CERTS"
for pair in conductor:conductor switch:switch1 switch:switch2 switch:switch3 agent:agent1 agent:agent2; do
    [ -f "$CERTS/${pair##*:}.pem" ] || "$ROOT/tools/vnet-certs.sh" issue "$CERTS" "${pair%%:*}" "${pair##*:}"
done
echo "certificates are in $CERTS"

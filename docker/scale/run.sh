#!/bin/bash
# One conductor, several switches, and many agents on a private compose network.
# Generated files stay in docker/scale/out and are not committed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/docker/scale/out"
SWITCHES="${SWITCHES:-10}"
AGENTS="${AGENTS:-100}"
PROJECT="${PROJECT:-vnetscale}"

mkdir -p "$OUT" "$ROOT/docker/certs"
if [ ! -f "$ROOT/docker/certs/cert.pem" ] || [ ! -f "$ROOT/docker/certs/key.pem" ]; then
    openssl req -x509 -newkey rsa:2048 \
        -keyout "$ROOT/docker/certs/key.pem" \
        -out "$ROOT/docker/certs/cert.pem" \
        -days 365 -nodes -subj /CN=vnet >/dev/null 2>&1
fi

python3 - "$OUT" "$SWITCHES" "$AGENTS" << 'PY'
import sys
from pathlib import Path
out, n_sw, n_ag = Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])

auth = []
for i in range(1, n_sw + 1):
    auth.append(f"switch switch{i} switchkey{i}")
for i in range(1, n_ag + 1):
    auth.append(f"agent agent{i} agentkey{i}")
(out / "auth.txt").write_text("\n".join(auth) + "\n")

cfg = [f"agent{i} 10.0.1.{i}" for i in range(1, n_ag + 1)]
cfg += ["agent1 agent2", "agent1 ::internet:8.8.8.8", "agent2 ::internet:8.8.8.8"]
(out / "switch.txt").write_text("\n".join(cfg) + "\n")

tls = """      - VNET_TLS=1
      - VNET_TLS_CERT=/etc/vnet/tls/cert.pem
      - VNET_TLS_KEY=/etc/vnet/tls/key.pem
      - VNET_TLS_CA=/etc/vnet/tls/cert.pem"""
certs = "      - ../../certs:/etc/vnet/tls:ro"

lines = [
    "services:",
    "  conductor:",
    "    image: vnet-conductor:scale",
    "    container_name: vnet-scale-conductor",
    "    networks: [vnet]",
    "    environment:",
    "      - VNET_AUTH_FILE=/etc/vnet/auth.txt",
    tls,
    "    volumes:",
    "      - ./auth.txt:/etc/vnet/auth.txt:ro",
    certs,
    "    command: ['/usr/local/bin/conductor']",
]
for i in range(1, n_sw + 1):
    port = 6000 + i - 1
    lines += [
        f"  switch{i}:",
        "    image: vnet-switch:scale",
        f"    container_name: vnet-scale-switch{i}",
        "    networks: [vnet]",
        "    depends_on: [conductor]",
        "    environment:",
        "      - CONDUCTOR_IP=conductor",
        "      - CONDUCTOR_PORT=5000",
        "      - VNET_CONFIG_PATH=/etc/vnet/config.txt",
        tls,
        "    volumes:",
        "      - ./switch.txt:/etc/vnet/config.txt:ro",
        certs,
        "    cap_add: [NET_ADMIN]",
        "    sysctls:",
        '      net.ipv4.ip_forward: "1"',
        '      net.ipv4.conf.all.rp_filter: "0"',
        "    devices:",
        "      - /dev/net/tun:/dev/net/tun",
        f"    command: ['/usr/local/bin/switch', 'switch{i}', 'switchkey{i}', '{port}']",
    ]
for i in range(1, n_ag + 1):
    lines += [
        f"  agent{i}:",
        "    image: vnet-agent:scale",
        f"    container_name: vnet-scale-agent{i}",
        "    networks: [vnet]",
        "    depends_on: [conductor]",
        "    environment:",
        "      - CONDUCTOR_IP=conductor",
        "      - CONDUCTOR_PORT=5000",
        tls,
        "    volumes:",
        certs,
        "    cap_add: [NET_ADMIN]",
        "    devices:",
        "      - /dev/net/tun:/dev/net/tun",
        f"    command: ['/usr/local/bin/agent', 'agent{i}', 'agentkey{i}']",
        "    restart: 'no'",
    ]
lines += ["networks:", "  vnet:", "    driver: bridge"]
(out / "compose.yml").write_text("\n".join(lines) + "\n")
print(f"wrote {n_sw} switches and {n_ag} agents")
PY

if [ -f "$OUT/compose.yml" ]; then
    docker compose -p "$PROJECT" -f "$OUT/compose.yml" down --remove-orphans || true
fi

echo "Building images..."
docker build -f "$ROOT/docker/Dockerfile.conductor" -t vnet-conductor:scale "$ROOT"
docker build -f "$ROOT/docker/Dockerfile.switch" -t vnet-switch:scale "$ROOT"
docker build -f "$ROOT/docker/Dockerfile.agent" -t vnet-agent:scale "$ROOT"

switch_services=()
for i in $(seq 1 "$SWITCHES"); do switch_services+=("switch$i"); done
docker compose -p "$PROJECT" -f "$OUT/compose.yml" up -d --force-recreate --remove-orphans \
    conductor "${switch_services[@]}"

echo "Waiting for $SWITCHES switches..."
deadline=$((SECONDS + 90))
up=0
while [ "$SECONDS" -lt "$deadline" ]; do
    up=$(docker logs vnet-scale-conductor 2>&1 | grep -c "Switch registered" || true)
    if [ "$up" -ge "$SWITCHES" ]; then
        break
    fi
    sleep 2
done
echo "switches=$up/$SWITCHES"
if [ "$up" -lt "$SWITCHES" ]; then
    echo "Switches did not all register" >&2
    exit 1
fi

agent_services=()
for i in $(seq 1 "$AGENTS"); do agent_services+=("agent$i"); done
docker compose -p "$PROJECT" -f "$OUT/compose.yml" up -d "${agent_services[@]}"

echo "Waiting for $AGENTS registrations..."
deadline=$((SECONDS + 180))
registered=0
while [ "$SECONDS" -lt "$deadline" ]; do
    registered=$(docker logs vnet-scale-conductor 2>&1 | grep -c "registered with IP" || true)
    if [ "$registered" -ge "$AGENTS" ]; then
        break
    fi
    sleep 3
done

echo "registered=$registered/$AGENTS"
echo "---- conductor problems ----"
docker logs vnet-scale-conductor 2>&1 | grep -E "Heartbeat timeout|Failed to send|No available switch" || true

if [ "$registered" -lt "$AGENTS" ]; then
    echo "Not every agent registered" >&2
    exit 1
fi

echo "---- cross-switch ping ----"
docker exec vnet-scale-agent1 ping -c 2 -W 3 10.0.1.2

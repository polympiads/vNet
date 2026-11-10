#!/bin/bash
# Integration tests: real conductor, switches and agents in Linux network
# namespaces, with TLS and per-machine certificates.
#
#   sudo tests/integration/run.sh <build dir> [scenario ...]
#
# Needs root (namespaces, TUN, iptables), iproute2, iputils-ping, python3
# and openssl; iperf3 for the throughput scenario, which is skipped
# without it. Logs go to <build dir>/integration/<scenario>/.
#
# Topology: namespace "core" holds the conductor and routes between the
# others; a1..a3 hold agents, s1 and s2 hold switches, each joined to core
# by a veth pair (core side .1, far side .2):
#   agents 172.31.<i>.2   switches 172.30.<i>.2
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$(cd "${1:?usage: run.sh <build dir> [scenario ...]}" && pwd)"
shift
OUT="$BIN/integration"
CERTS="$OUT/certs"
NSS="core a1 a2 a3 s1 s2"
HB=(VNET_HEARTBEAT_SEC=2 VNET_DEAD_SEC=6)
declare -A PID
LOGS=""
FAILED=0

[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 2; }
for tool in ip ping python3 openssl iptables; do
  command -v $tool >/dev/null || { echo "missing $tool"; exit 2; }
done
mkdir -p "$OUT"

# ---------------------------------------------------------------- helpers

say()  { echo "    $*"; }
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1: $2"; FAILED=1; }

make_certs() {
  [ -f "$CERTS/ca.pem" ] && return
  "$HERE/../../tools/vnet-certs.sh" init "$CERTS" >/dev/null
  for pair in conductor:conductor switch:switch1 switch:switch2 agent:agent1 agent:agent2 agent:agent3; do
    "$HERE/../../tools/vnet-certs.sh" issue "$CERTS" "${pair%%:*}" "${pair##*:}" >/dev/null
  done
}

tls_env() {  # name
  echo "VNET_TLS=1 VNET_TLS_CA=$CERTS/ca.pem VNET_TLS_CERT=$CERTS/$1.pem VNET_TLS_KEY=$CERTS/$1.key"
}

topo_up() {  # scenario name
  LOGS="$OUT/$1"
  rm -rf "$LOGS"; mkdir -p "$LOGS"
  for n in $NSS; do ip netns del $n 2>/dev/null; ip netns add $n; ip netns exec $n ip link set lo up; done
  ip netns exec core sysctl -qw net.ipv4.ip_forward=1
  link() {
    ip link add x$2 type veth peer name c$2
    ip link set x$2 netns $1
    ip link set c$2 netns core
    ip netns exec core ip addr add $3.1/24 dev c$2
    ip netns exec core ip link set c$2 up
    ip netns exec $1 ip addr add $3.2/24 dev x$2
    ip netns exec $1 ip link set x$2 up
    ip netns exec $1 ip route add default via $3.1
  }
  link a1 a1 172.31.1; link a2 a2 172.31.2; link a3 a3 172.31.3
  link s1 s1 172.30.1; link s2 s2 172.30.2
  cat > "$LOGS/switch.txt" <<EOF
agent1 10.0.1.1
agent2 10.0.1.2
agent3 10.0.1.3
agent1 agent2
agent1 agent3
EOF
  cat > "$LOGS/auth.txt" <<EOF
switch switch1 key1
switch switch2 key2
agent agent1 k1
agent agent2 k2
agent agent3 k3
EOF
}

topo_down() {
  for k in "${!PID[@]}"; do kill "${PID[$k]}" 2>/dev/null; done
  wait 2>/dev/null
  PID=()
  for n in $NSS; do ip netns del $n 2>/dev/null; done
}
trap topo_down EXIT

# Processes, all with TLS and their own certificate.
CONDUCTOR_ENV=()   # extra settings for the conductor of one scenario
start_conductor() {
  ip netns exec core env "${HB[@]}" $(tls_env conductor) VNET_AUTH_FILE="$LOGS/auth.txt" \
      VNET_DUMP_PATH="$LOGS/dump.bin" "${CONDUCTOR_ENV[@]}" "$BIN/conductor" >> "$LOGS/conductor.log" 2>&1 &
  PID[conductor]=$!
}
SWITCH_ENV=()   # extra settings for the switches of one scenario
start_switch() {  # i [cert name]; SWITCH_CONFIG set but empty = no file of its own
  ip netns exec s$1 env "${HB[@]}" $(tls_env ${2:-switch$1}) CONDUCTOR_IP=172.30.$1.1 CONDUCTOR_PORT=5000 \
      VNET_CONFIG_PATH="${SWITCH_CONFIG-$LOGS/switch.txt}" VNET_AUTH_KEY=key$1 VNET_STATS_SEC=0 \
      "${SWITCH_ENV[@]}" "$BIN/switch" switch$1 600$1 >> "$LOGS/switch$1.log" 2>&1 &
  PID[switch$1]=$!
}
start_agent() {  # i [extra env...]
  local i=$1; shift
  ip netns exec a$i env "${HB[@]}" $(tls_env agent$i) CONDUCTOR_IP=172.31.$i.1 CONDUCTOR_PORT=5000 \
      VNET_AUTH_KEY=k$i VNET_STATE_DIR="$LOGS/state" "$@" "$BIN/agent" agent$i >> "$LOGS/agent$i.log" 2>&1 &
  PID[agent$i]=$!
}
stop() { kill -"${2:-TERM}" "${PID[$1]}" 2>/dev/null; wait "${PID[$1]}" 2>/dev/null; unset PID[$1]; }
alive() { kill -0 "${PID[$1]}" 2>/dev/null; }

# wait_for <file> <pattern> <count> <seconds>
wait_for() {
  local deadline=$((SECONDS + $4))
  while [ $SECONDS -lt $deadline ]; do
    [ "$(grep -c -- "$2" "$1" 2>/dev/null)" -ge "$3" ] && return 0
    sleep 0.5
  done
  return 1
}
# pings <from agent i> <ip> <count> -> prints received count
pings() {
  ip netns exec a$1 ping -c "$3" -i 0.2 -W 1 "$2" 2>/dev/null | grep -o '[0-9]* received' | cut -d' ' -f1
}
# in_ns <ns> <cmd...>
in_ns() { local n=$1; shift; ip netns exec $n "$@"; }

# Bring up conductor, two switches and two agents; return 1 on failure.
standard_start() {
  start_conductor; sleep 0.5
  start_switch 1; start_switch 2; sleep 1.5
  start_agent 1; start_agent 2; sleep 2
  wait_for "$LOGS/conductor.log" "registered with IP" 2 15
}

# ---------------------------------------------------------------- scenarios

scenario_basic() {
  topo_up basic
  standard_start || { fail basic "agents did not register"; return; }
  start_agent 3; sleep 2
  wait_for "$LOGS/conductor.log" "registered with IP" 3 10 || { fail basic "agent3 did not register"; return; }
  local r12 r13 r23
  r12=$(pings 1 10.0.1.2 3); r13=$(pings 1 10.0.1.3 3); r23=$(pings 2 10.0.1.3 3)
  say "allowed pairs: a1->a2 $r12/3, a1->a3 $r13/3; denied pair a2->a3 $r23/3"
  [ "$r12" = 3 ] && [ "$r13" = 3 ] || { fail basic "allowed pair could not ping"; return; }
  [ "${r23:-0}" = 0 ] || { fail basic "ACL-denied pair could ping"; return; }
  grep -q "role in OU checked" "$LOGS/conductor.log" || { fail basic "TLS role check not on"; return; }
  "$BIN/vnet-state" "$LOGS/dump.bin" "$LOGS/auth.txt" > "$LOGS/state.txt" 2>&1 || true
  pass basic
}

scenario_attacks() {
  topo_up attacks
  # Conductor and switch share the core namespace on loopback, in plain
  # TCP, so the raw client can speak the protocol to both.
  ip netns exec core env "${HB[@]}" VNET_ALLOW_INSECURE=1 VNET_AUTH_FILE="$LOGS/auth.txt" \
      VNET_DUMP_PATH="$LOGS/dump.bin" "$BIN/conductor" >> "$LOGS/conductor.log" 2>&1 &
  PID[conductor]=$!
  sleep 0.5
  ip netns exec core env "${HB[@]}" VNET_ALLOW_INSECURE=1 CONDUCTOR_IP=127.0.0.1 CONDUCTOR_PORT=5000 \
      VNET_CONFIG_PATH="$LOGS/switch.txt" VNET_AUTH_KEY=key1 VNET_STATS_SEC=0 \
      "$BIN/switch" switch1 6000 >> "$LOGS/switch1.log" 2>&1 &
  PID[switch1]=$!
  sleep 1.5
  wait_for "$LOGS/conductor.log" "Switch registered" 1 10 || { fail attacks "switch did not register"; return; }
  local ok=1 line
  for check in oversized forged_token forged_hello forged_withdrawal forged_config reidentify bad_key unknown_name; do
    line=$(ip netns exec core python3 "$HERE/client.py" $check 2>&1); say "$line"
    echo "$line" | grep -q "RESULT $check ok" || ok=0
  done
  line=$(ip netns exec core python3 "$HERE/client.py" idle 14 2>&1); say "$line"
  echo "$line" | grep -q "RESULT idle ok" || ok=0
  alive conductor && alive switch1 || { fail attacks "a process died"; return; }
  [ $ok = 1 ] && pass attacks || fail attacks "see lines above"
}

scenario_conductor_restart() {
  topo_up conductor_restart
  standard_start || { fail conductor_restart "agents did not register"; return; }
  [ "$(pings 1 10.0.1.2 3)" = 3 ] || { fail conductor_restart "initial ping"; return; }
  stop conductor KILL; sleep 2; start_conductor
  wait_for "$LOGS/conductor.log" "Switch registered" 4 20 || { fail conductor_restart "switches did not re-register"; return; }
  wait_for "$LOGS/conductor.log" "Agent reconnected" 2 20 || { fail conductor_restart "agents did not rejoin"; return; }
  [ "$(pings 1 10.0.1.2 3)" = 3 ] || { fail conductor_restart "ping after restart"; return; }
  start_agent 3; sleep 3
  [ "$(pings 3 10.0.1.1 3)" = 3 ] || { fail conductor_restart "new agent after restart"; return; }
  say "both switches re-registered, both agents rejoined, agent3 joined"
  pass conductor_restart
}

scenario_switch_restart() {
  topo_up switch_restart
  standard_start || { fail switch_restart "agents did not register"; return; }
  [ "$(pings 1 10.0.1.2 3)" = 3 ] || { fail switch_restart "initial ping"; return; }
  stop switch1 KILL; sleep 2; start_switch 1
  wait_for "$LOGS/conductor.log" "registered with IP" 3 45 || { fail switch_restart "agent did not rejoin"; return; }
  sleep 1
  [ "$(pings 1 10.0.1.2 3)" = 3 ] && [ "$(pings 2 10.0.1.1 3)" = 3 ] || { fail switch_restart "ping after restart"; return; }
  say "agent behind the killed switch rejoined; pings both ways"
  pass switch_restart
}

scenario_switch_link() {
  topo_up switch_link
  standard_start || { fail switch_link "agents did not register"; return; }
  in_ns core ip link set cs1 down; sleep 12; in_ns core ip link set cs1 up
  wait_for "$LOGS/conductor.log" "registered with IP" 3 45 || { fail switch_link "agent did not rejoin"; return; }
  sleep 1
  [ "$(pings 1 10.0.1.2 3)" = 3 ] || { fail switch_link "ping after link cut"; return; }
  say "switch1 re-registered after a 12 s cut; agent behind it rejoined"
  pass switch_link
}

scenario_agent_exit() {
  topo_up agent_exit
  standard_start || { fail agent_exit "agents did not register"; return; }
  in_ns a1 ip route | grep -q "^default dev vnet-agent1" || { fail agent_exit "default route not via TUN while joined"; return; }
  stop agent1 TERM; sleep 1
  in_ns a1 ip route | grep -q "^default via 172.31.1.1" || { fail agent_exit "default route not restored"; return; }
  in_ns a1 ip route | grep -q "via 172.31.1.1 dev xa1$" && { fail agent_exit "pin left behind"; return; }
  say "default route restored, pins removed"
  pass agent_exit
}

scenario_lockdown() {
  topo_up lockdown
  in_ns core ip addr add 172.31.1.9/24 dev ca1
  start_conductor; sleep 0.5
  start_switch 1; start_switch 2; sleep 1.5
  start_agent 1 VNET_LOCKDOWN=1 VNET_LOCKDOWN_ALLOW=172.31.1.9/32
  start_agent 2; sleep 2
  wait_for "$LOGS/conductor.log" "registered with IP" 2 15 || { fail lockdown "agents did not register"; return; }
  [ "$(pings 1 10.0.1.2 2)" = 2 ] || { fail lockdown "overlay blocked"; return; }
  in_ns a1 ping -c 1 -W 1 172.31.1.1 >/dev/null 2>&1 && { fail lockdown "LAN gateway reachable"; return; }
  in_ns a1 ping -c 1 -W 1 172.31.1.9 >/dev/null 2>&1 || { fail lockdown "allowed host blocked"; return; }
  in_ns core ping -c 1 -W 1 -I 172.31.1.1 172.31.1.2 >/dev/null 2>&1 && { fail lockdown "inbound from LAN reachable"; return; }
  in_ns core ping -c 1 -W 1 -I 172.31.1.9 172.31.1.2 >/dev/null 2>&1 || { fail lockdown "inbound from allowed host blocked"; return; }
  stop agent1 TERM; sleep 1
  in_ns a1 ping -c 1 -W 1 172.31.1.1 >/dev/null 2>&1 || { fail lockdown "gateway still blocked after exit"; return; }
  [ "$(in_ns a1 iptables -S | grep -c VNET)" = 0 ] || { fail lockdown "rules left behind"; return; }
  say "LAN blocked, overlay and allowed host open, all removed on exit"
  pass lockdown
}

# A TCP listener in a namespace, kept in PID under a name.
listen_tcp() {  # ns name port
  ip netns exec $1 python3 -c '
import socket, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", int(sys.argv[1]))); s.listen(5); time.sleep(120)' $3 &
  PID[$2]=$!
}
# connects <ns> <source ip> <dest ip> <port>: 0 when a TCP connection completes within 2 s
connects() {
  ip netns exec $1 python3 -c '
import socket, sys
try:
    socket.create_connection((sys.argv[2], int(sys.argv[3])), timeout=2, source_address=(sys.argv[1], 0)).close()
except OSError:
    sys.exit(1)' $2 $3 $4 2>/dev/null
}

# The bypass file: a broker in another subnet stays reachable through
# the physical gateway, with and without lockdown; in/out rules open
# single ports; a reload follows the file; everything goes on exit.
scenario_bypass() {
  topo_up bypass
  in_ns core ip addr add 172.29.0.9/24 dev lo
  start_conductor; sleep 0.5
  start_switch 1; start_switch 2; sleep 1.5
  cat > "$LOGS/bypass.conf" <<EOF
# the broker's subnet stays on the physical network
route 172.29.0.0/24
# the gateway may reach the service on 2244, nothing else
in -p tcp -s 172.31.1.1/32 --dport 2244 -j ACCEPT
# this machine may reach the gateway's service on 2255
out -p tcp -d 172.31.1.1/32 --dport 2255 -j ACCEPT
in6 -p tcp --dport 2266 -j ACCEPT
EOF
  start_agent 1 VNET_LOCKDOWN=1 VNET_BYPASS_FILE="$LOGS/bypass.conf"
  start_agent 2 VNET_BYPASS_FILE="$LOGS/bypass.conf"
  sleep 2
  wait_for "$LOGS/conductor.log" "registered with IP" 2 15 || { fail bypass "agents did not register"; return; }
  [ "$(pings 1 10.0.1.2 2)" = 2 ] || { fail bypass "overlay blocked"; return; }
  # The broker is off the local subnet: without the route line it would
  # go into the TUN. Reachable both ways, with lockdown (a1) and without (a2).
  in_ns a1 ping -c 1 -W 1 172.29.0.9 >/dev/null 2>&1 || { fail bypass "broker unreachable under lockdown"; return; }
  in_ns a2 ping -c 1 -W 1 172.29.0.9 >/dev/null 2>&1 || { fail bypass "broker unreachable without lockdown"; return; }
  in_ns core ping -c 1 -W 1 -I 172.29.0.9 172.31.1.2 >/dev/null 2>&1 || { fail bypass "broker cannot reach the agent"; return; }
  in_ns a1 ip route | grep -q "^172.29.0.0/24 via 172.31.1.1 dev xa1" || { fail bypass "route not pinned"; return; }
  in_ns a1 ping -c 1 -W 1 172.31.1.1 >/dev/null 2>&1 && { fail bypass "LAN gateway reachable"; return; }
  # Port rules: the gateway reaches a1 only on 2244; a1 reaches the gateway only on 2255.
  listen_tcp a1 l2244 2244; listen_tcp a1 l2245 2245; listen_tcp core l2255 2255; listen_tcp core l2256 2256; sleep 0.5
  connects core 172.31.1.1 172.31.1.2 2244 || { fail bypass "allowed inbound port blocked"; return; }
  connects core 172.31.1.1 172.31.1.2 2245 && { fail bypass "other inbound port open"; return; }
  connects a1 172.31.1.2 172.31.1.1 2255 || { fail bypass "allowed outbound port blocked"; return; }
  connects a1 172.31.1.2 172.31.1.1 2256 && { fail bypass "other outbound port open"; return; }
  in_ns a1 ip6tables -S VNET_IN | grep -q -- "--dport 2266" || { fail bypass "in6 rule missing"; return; }
  # Reload: the route line leaves the file and comes back.
  cp "$LOGS/bypass.conf" "$LOGS/full.conf"
  grep -v '^route' "$LOGS/full.conf" > "$LOGS/bypass.conf"
  kill -HUP "${PID[agent1]}"; sleep 1
  grep -q "Bypass file reloaded" "$LOGS/agent1.log" || { fail bypass "no reload"; return; }
  in_ns a1 ping -c 1 -W 1 172.29.0.9 >/dev/null 2>&1 && { fail bypass "broker still reachable after the route left the file"; return; }
  in_ns a1 ip route | grep -q "^172.29.0.0/24" && { fail bypass "pin kept after reload"; return; }
  cp "$LOGS/full.conf" "$LOGS/bypass.conf"
  kill -HUP "${PID[agent1]}"; sleep 1
  in_ns a1 ping -c 1 -W 1 172.29.0.9 >/dev/null 2>&1 || { fail bypass "broker unreachable after the route came back"; return; }
  # A broken file changes nothing.
  printf 'route 0.0.0.0/0\n' >> "$LOGS/bypass.conf"
  kill -HUP "${PID[agent1]}"; sleep 1
  grep -q "reload failed" "$LOGS/agent1.log" || { fail bypass "bad file not reported"; return; }
  in_ns a1 ping -c 1 -W 1 172.29.0.9 >/dev/null 2>&1 || { fail bypass "broker lost after a bad reload"; return; }
  # Exit: pins and chains gone, on both machines.
  stop agent1 TERM; stop agent2 TERM; sleep 1
  in_ns a1 ip route | grep -q "^172.29.0.0/24" && { fail bypass "pin left behind"; return; }
  in_ns a2 ip route | grep -q "^172.29.0.0/24" && { fail bypass "pin left behind without lockdown"; return; }
  [ "$(in_ns a1 iptables -S | grep -c VNET)" = 0 ] || { fail bypass "rules left behind"; return; }
  say "broker in another subnet reachable through the pin; port rules hold; reload and exit clean"
  pass bypass
}

# Switches with config files of their own: compared, reloaded on SIGHUP.
scenario_config_files() {
  topo_up config_files
  start_conductor; sleep 0.5
  start_switch 1
  printf 'agent1 10.0.1.1\nagent2 10.0.1.2\nagent3 10.0.1.3\n' > "$LOGS/other.txt"
  SWITCH_CONFIG="$LOGS/other.txt" start_switch 2; unset SWITCH_CONFIG
  sleep 3
  [ "$(grep -c 'Switch registered' "$LOGS/conductor.log")" = 1 ] || { fail config_files "mismatched config not refused"; return; }
  grep -q "differs from" "$LOGS/conductor.log" || { fail config_files "no mismatch message"; return; }
  stop switch2 TERM
  # Reload: agent2<->agent3 not allowed, then allowed after SIGHUP.
  start_switch 2; sleep 1.5
  start_agent 2; start_agent 3; sleep 2
  wait_for "$LOGS/conductor.log" "registered with IP" 2 15 || { fail config_files "agents did not register"; return; }
  [ "${r:=$(pings 2 10.0.1.3 1)}" = 0 ] || { fail config_files "denied pair pinged before reload"; return; }
  printf 'agent2 agent3\n' >> "$LOGS/switch.txt"
  kill -HUP "${PID[switch1]}" "${PID[switch2]}"; sleep 1
  [ "$(pings 2 10.0.1.3 3)" = 3 ] || { fail config_files "pair still denied after reload"; return; }
  printf 'garbage\n' >> "$LOGS/switch.txt"
  kill -HUP "${PID[switch1]}"; sleep 1
  grep -q "reload failed" "$LOGS/switch1.log" || { fail config_files "bad reload not reported"; return; }
  [ "$(pings 2 10.0.1.3 3)" = 3 ] || { fail config_files "old config not kept"; return; }
  say "mismatch refused; SIGHUP applied; broken file kept the old config"
  pass config_files
}

# The conductor holds the config: switches get it as they register and
# after a SIGHUP on the conductor; a file larger than one frame arrives
# in pieces.
scenario_config() {
  topo_up config
  CONDUCTOR_ENV=(VNET_CONFIG_PATH="$LOGS/switch.txt"); start_conductor; CONDUCTOR_ENV=(); sleep 0.5
  # switch1 has no file of its own; switch2 starts from a stale one.
  SWITCH_CONFIG= start_switch 1
  printf 'agent1 10.0.1.1\nagent2 10.0.1.2\nagent3 10.0.1.3\n' > "$LOGS/other.txt"
  SWITCH_CONFIG="$LOGS/other.txt" start_switch 2; unset SWITCH_CONFIG
  wait_for "$LOGS/conductor.log" "applied config" 2 10 || { fail config "switches did not confirm the config"; return; }
  grep -q "waiting for the config from the conductor" "$LOGS/switch1.log" || { fail config "switch1 did not wait for the config"; return; }
  start_agent 2; start_agent 3; sleep 2
  wait_for "$LOGS/conductor.log" "registered with IP" 2 15 || { fail config "agents did not register"; return; }
  [ "$(pings 2 10.0.1.3 1)" = 0 ] || { fail config "denied pair pinged before reload"; return; }
  # One edit on the conductor reaches both switches.
  printf 'agent2 agent3\n' >> "$LOGS/switch.txt"
  kill -HUP "${PID[conductor]}"
  wait_for "$LOGS/conductor.log" "applied config" 4 10 || { fail config "reload did not reach both switches"; return; }
  [ "$(pings 2 10.0.1.3 3)" = 3 ] || { fail config "pair still denied after reload"; return; }
  # A broken file keeps the old config everywhere.
  cp "$LOGS/switch.txt" "$LOGS/good.txt"
  printf 'garbage\n' >> "$LOGS/switch.txt"
  kill -HUP "${PID[conductor]}"; sleep 1
  grep -q "Config reload failed" "$LOGS/conductor.log" || { fail config "bad reload not reported"; return; }
  [ "$(pings 2 10.0.1.3 3)" = 3 ] || { fail config "old config not kept"; return; }
  # A config larger than one frame (5000 more machines) arrives whole.
  { cat "$LOGS/good.txt"; for i in $(seq 1 5000); do printf 'f%04d 10.9.%d.%d\n' $i $((i / 250)) $((i % 250 + 1)); done; } > "$LOGS/switch.txt"
  kill -HUP "${PID[conductor]}"
  wait_for "$LOGS/conductor.log" "applied config" 6 15 || { fail config "large config did not reach both switches"; return; }
  grep -q "5003 agents" "$LOGS/switch1.log" || { fail config "large config not applied whole"; return; }
  [ "$(pings 2 10.0.1.3 3)" = 3 ] || { fail config "pair denied after the large config"; return; }
  # A switch that restarts gets the config again, without a file.
  stop switch1 TERM; SWITCH_CONFIG= start_switch 1; unset SWITCH_CONFIG
  wait_for "$LOGS/conductor.log" "applied config" 7 15 || { fail config "restarted switch did not get the config"; return; }
  local bytes
  bytes=$(grep -o "applied, fingerprint [0-9a-f]* ([0-9]* bytes" "$LOGS/switch1.log" | tail -1 | grep -o "[0-9]* bytes")
  say "config sent on registration and on SIGHUP; broken file kept; large config ($bytes) in pieces"
  pass config
}

scenario_auth_reload() {
  topo_up auth_reload
  # agent3 is not in the auth file yet.
  grep -v agent3 "$LOGS/auth.txt" > "$LOGS/auth2.txt" && mv "$LOGS/auth2.txt" "$LOGS/auth.txt"
  standard_start || { fail auth_reload "agents did not register"; return; }
  start_agent 3; sleep 3
  grep -q "Rejected agent auth: agent3" "$LOGS/conductor.log" || { fail auth_reload "unknown agent not refused"; return; }
  printf 'agent agent3 k3\n' >> "$LOGS/auth.txt"
  kill -HUP "${PID[conductor]}"
  wait_for "$LOGS/conductor.log" "Auth file reloaded" 1 5 || { fail auth_reload "no reload"; return; }
  wait_for "$LOGS/conductor.log" "agent3 registered with IP" 1 15 || { fail auth_reload "agent3 did not join after reload"; return; }
  [ "$(pings 1 10.0.1.3 3)" = 3 ] || { fail auth_reload "agent3 unreachable"; return; }
  printf 'garbage\n' >> "$LOGS/auth.txt"
  kill -HUP "${PID[conductor]}"; sleep 1
  grep -q "reload failed" "$LOGS/conductor.log" || { fail auth_reload "bad file not reported"; return; }
  alive conductor || { fail auth_reload "conductor died"; return; }
  say "agent3 refused, then joined after SIGHUP; a broken file kept the old records"
  pass auth_reload
}

scenario_agent_crash() {
  topo_up agent_crash
  standard_start || { fail agent_crash "agents did not register"; return; }
  # A crash restores nothing: the machine is left with the TUN gone and
  # no default route. The next agent must put the physical route back.
  stop agent1 KILL; sleep 1
  in_ns a1 ip route | grep -q "^default" && { fail agent_crash "a killed agent should leave no default route"; return; }
  start_agent 1; sleep 3
  wait_for "$LOGS/agent1.log" "Restored the default route" 1 10 || { fail agent_crash "saved route not used"; return; }
  wait_for "$LOGS/conductor.log" "agent1 registered with IP" 2 15 || { fail agent_crash "agent1 did not rejoin"; return; }
  [ "$(pings 1 10.0.1.2 3)" = 3 ] || { fail agent_crash "ping after restart"; return; }
  stop agent1 TERM; sleep 1
  in_ns a1 ip route | grep -q "^default via 172.31.1.1" || { fail agent_crash "default route not restored after the restart"; return; }
  say "after SIGKILL the next agent restored the gateway from its saved state"
  pass agent_crash
}

scenario_duplicate_identity() {
  topo_up duplicate_identity
  start_conductor; sleep 0.5
  start_switch 1; sleep 1.5
  start_agent 1; start_agent 2; sleep 2
  wait_for "$LOGS/conductor.log" "registered with IP" 2 15 || { fail duplicate_identity "agents did not register"; return; }
  # agent1 freezes (a VM paused, say) and a machine with the same identity
  # appears in a3. Its sockets are silent, not closed: the newcomer must
  # replace them on the switch and on the conductor.
  kill -STOP "${PID[agent1]}"
  ip netns exec a3 env "${HB[@]}" $(tls_env agent1) CONDUCTOR_IP=172.31.3.1 CONDUCTOR_PORT=5000 \
      VNET_AUTH_KEY=k1 VNET_STATE_DIR="$LOGS/state-a3" "$BIN/agent" agent1 >> "$LOGS/agent1-copy.log" 2>&1 &
  PID[agent1copy]=$!
  wait_for "$LOGS/switch1.log" "authenticated again; closing" 1 15 || { fail duplicate_identity "switch kept the frozen socket"; return; }
  wait_for "$LOGS/conductor.log" "registered again; closing" 1 15 || { fail duplicate_identity "conductor kept the frozen socket"; return; }
  sleep 1
  [ "$(pings 3 10.0.1.2 3)" = 3 ] || { fail duplicate_identity "the newcomer cannot ping"; return; }
  kill -CONT "${PID[agent1]}"; stop agent1 KILL
  say "a second machine with agent1's identity took over both sockets"
  pass duplicate_identity
}

scenario_flood() {
  command -v iperf3 >/dev/null || { echo "SKIP flood (no iperf3)"; return; }
  topo_up flood
  # Fixed placement through networks: switch1 serves room1 (agent1 and
  # agent3), switch2 serves room2 (agent2). Low limits, so the flooded
  # agent gets little even on a small machine.
  start_conductor; sleep 0.5
  SWITCH_ENV=(VNET_AGENT_PPS=5000 VNET_AGENT_MBPS=50 VNET_NETWORK=room1); start_switch 1
  SWITCH_ENV=(VNET_AGENT_PPS=5000 VNET_AGENT_MBPS=50 VNET_NETWORK=room2); start_switch 2
  SWITCH_ENV=(); sleep 1.5
  start_agent 1 VNET_NETWORK=room1; start_agent 2 VNET_NETWORK=room2; start_agent 3 VNET_NETWORK=room1; sleep 2
  wait_for "$LOGS/conductor.log" "registered with IP" 3 15 || { fail flood "agents did not register"; return; }
  grep -q "agent3 registered with IP 10.0.1.3 on switch switch1" "$LOGS/conductor.log" || { fail flood "placement by network did not hold"; return; }
  # agent3 floods agent1 on the same switch; agent2 pings agent1 from the
  # other switch meanwhile. The flooder's own agent is busy, so the
  # victim is the one to measure.
  in_ns a1 iperf3 -s -1 -p 5201 > "$LOGS/iperf-server.log" 2>&1 &
  sleep 0.5
  in_ns a3 iperf3 -c 10.0.1.1 -u -b 300M -l 1400 -t 5 -p 5201 > "$LOGS/iperf.log" 2>&1 &
  local iperf=$!
  sleep 1
  local got
  got=$(pings 2 10.0.1.1 15)
  wait $iperf
  kill -USR1 "${PID[switch1]}" "${PID[switch2]}"; sleep 0.5
  local limited
  limited=$(grep -h "Stats:" "$LOGS"/switch*.log | grep -o "rate-limited=[0-9]*" | cut -d= -f2 | paste -sd+ | bc)
  say "pings from agent2 to the flooded agent1: $got/15; rate-limited packets: ${limited:-0}"
  [ "${limited:-0}" -gt 0 ] || { fail flood "nothing was rate limited"; return; }
  [ "${got:-0}" -ge 12 ] || { fail flood "pings lost during flood"; return; }
  pass flood
}

scenario_throughput() {
  command -v iperf3 >/dev/null || { echo "SKIP throughput (no iperf3)"; return; }
  topo_up throughput
  standard_start || { fail throughput "agents did not register"; return; }
  in_ns a2 iperf3 -s -1 -p 5201 > "$LOGS/iperf-server.log" 2>&1 &
  sleep 0.5
  in_ns a1 iperf3 -c 10.0.1.2 -t 3 -p 5201 > "$LOGS/iperf.log" 2>&1
  local line mbit
  line=$(grep receiver "$LOGS/iperf.log" | tail -1)
  mbit=$(echo "$line" | awk '{for (i=1;i<=NF;i++) if ($i ~ /bits\/sec/) {v=$(i-1); u=$i}} END {if (u ~ /^G/) v*=1000; if (u ~ /^K/) v/=1000; printf "%d", v}')
  say "agent1 -> agent2 across two switches: ${mbit:-0} Mbit/s"
  [ "${mbit:-0}" -ge 50 ] || { fail throughput "below 50 Mbit/s"; return; }
  pass throughput
}

# ---------------------------------------------------------------- main

make_certs
ALL="basic attacks conductor_restart switch_restart switch_link agent_exit agent_crash duplicate_identity lockdown bypass config config_files auth_reload flood throughput"
for s in ${@:-$ALL}; do
  echo "== $s"
  topo_down
  "scenario_$s"
  topo_down
done
[ $FAILED = 0 ] && echo "all integration scenarios passed" || echo "some integration scenarios FAILED"
exit $FAILED

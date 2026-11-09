# polympiads/vNet - Virtualized Network

The purpose of the vNet software is to provide us with a way to link the virtual machines (both the ones used by our contestants and the ones used by host and runners), as part of a unified virtual network with security groups and network forwarding to the global internet.

## Setup

Three processes. One conductor, one or more switches, and one agent per machine. Start them in any order: a switch or an agent that cannot reach the conductor keeps trying every 2 seconds, and an agent that loses its switch asks the conductor for another.

The conductor listens on `CONDUCTOR_PORT` (5000 when unset). It has no arguments.

```
conductor
```

A switch registers with the conductor, then listens for agents on the port you pass it.

```
switch <name> <port>
```

An agent asks the conductor for a switch, then opens a TUN named `vnet-<name>`. The name must be at most 15 characters, so the agent name itself must be at most 10. Names are made of `[A-Za-z0-9_-]`.

```
agent <name>
```

Each switch and agent reads its key from `VNET_AUTH_KEY`, or from the first line of the file named by `VNET_AUTH_KEY_FILE`. The old forms `switch <name> <auth_key> <port>` and `agent <name> <auth_key>` still work but print a warning, because `ps` shows the key to every local user.

`CONDUCTOR_IP` and `CONDUCTOR_PORT` are required on every switch and every agent. Use an address that is reachable on the physical network. Switches and agents also need `CAP_NET_ADMIN` and `/dev/net/tun`. A switch needs `net.ipv4.ip_forward=1` and `rp_filter=0` on its egress interface. In-container `sysctl -w` is not enough when `/proc/sys` is read-only; set the sysctls from outside.

`cmake --install build` puts `conductor`, `switch`, `agent`, `vnet-state`, `vnet-certs` and `vnet-roster` in `bin/` and three systemd units in `lib/systemd/system/`. The units read `/etc/vnet/conductor.env`, `/etc/vnet/switch.env` and `/etc/vnet/agent.env`, which `vnet-roster` writes (see "Generating the files").

## Auth and TLS

Set `VNET_AUTH_FILE` on the conductor. Each line is `switch <name> <key>` or `agent <name> <key>`. The name and key must match what the process was given. Blank lines and `#` comments are ignored. Without the file the conductor refuses to start, since it would accept any name and key; `VNET_ALLOW_INSECURE=1` overrides that for a lab.

Set `VNET_TLS=1` on the conductor, every switch, and every agent, or on none of them. Without it every process refuses to start, since keys and routes would cross the network in the clear; `VNET_ALLOW_INSECURE=1` overrides that too. The three variables `VNET_TLS_CERT`, `VNET_TLS_KEY`, and `VNET_TLS_CA` are required when TLS is on.

Every certificate names its role in the subject's `OU` field: `conductor`, `switch` or `agent`. Each side checks the role of the peer it talks to, so the files on a contestant machine cannot be used to run a switch or a conductor. Hostnames are not checked. `tools/vnet-certs.sh init <dir>` makes a CA, `tools/vnet-certs.sh issue <dir> <role> <name>` makes a signed certificate; keep `ca.key` off every machine. `VNET_TLS_ROLE_CHECK=0` accepts certificates without a role, for a deployment that still uses one certificate everywhere.

Beyond keys and certificates, the switch accepts a token, a route or a peer notice only from its conductor link; a switch accepts another switch as a peer only with the token the conductor issued for that pair; a connection that has not identified itself within 10 seconds is closed; a header that announces more than 65994 bytes closes the connection; a connection identifies itself once.

## Switch config

`VNET_CONFIG_PATH` is required. Copy the same file to every switch: each switch applies the ACL itself and must know every virtual address. The conductor refuses a switch whose config differs from the registered switches' (compared by a fingerprint of the parsed content, so comments and ordering do not matter), and names both switches in its log. `VNET_CONFIG_HASH_CHECK=0` turns that off.

Three kinds of lines:

```
agent1  10.0.1.1
agent1  agent2
agent1  ::internet:8.8.8.8
agent1  ::internet:198.51.100.0/24
```

The first assigns a virtual address inside the overlay (`10.0.0.0/8` unless `VNET_OVERLAY_CIDR` is set). Names and addresses must be unique.

`agent1 agent2` allows those two machines to talk. The rule is bidirectional.

`agent1 ::internet:8.8.8.8` allows that machine to exchange packets with that one internet address, through the switch; `a.b.c.d/n` allows a whole prefix, and `0.0.0.0/0` allows everything. There is no rule for every internet host. Return traffic is accepted only for an address that was allowed outbound. If contestants should use their machine's normal route for general internet, set `VNET_DEFAULT_VIA_TUN=0` on the agent. Otherwise the agent replaces the default route with the TUN, and only listed internet addresses pass. Remember the resolver: a machine with no `::internet` rule for its DNS server resolves nothing.

Send `SIGHUP` to a switch to reload the file without a restart (`systemctl reload vnet-switch`). Rules apply to the next packet; a changed or removed address takes effect when that agent next connects. A file that does not parse leaves the previous config in force. Reload every switch, or the conductor logs that they disagree.

A switch announces itself as network `::internet` (`VNET_NETWORK`), so the conductor may assign any agent to it; an agent announces `vnet.internal` (`VNET_NETWORK`). The conductor assigns an agent to a switch of the same network or of `::internet`, round-robin across the switches that are up.

Allowed internet traffic is masqueraded out of the switch's physical default route. The switch TUN is `vnet-out` at `192.0.2.1/32`. `VNET_ADVERTISE_IP` is the address agents and peers dial when the one the conductor sees is not it (NAT, several interfaces).

## Routes that must stay off the TUN

After the TUN comes up, the agent pins the conductor address and the assigned switch address as `/32` routes via the previous physical gateway. Overlay addresses go through the TUN. Do not give the conductor or a switch an address inside `VNET_OVERLAY_CIDR`. When the agent stops it puts the default route back and removes the pins.

## Lockdown of contestant machines

The overlay only governs what enters the TUN. Two contestants on the same LAN can still reach each other directly, and a dual-stack network leaves the internet open over IPv6. With `VNET_LOCKDOWN=1` the agent adds host firewall rules (chains `VNET_IN` and `VNET_OUT`, hooked first in `INPUT` and `OUTPUT`) that allow only loopback, the TUN, the links to the conductor and the switch, replies to allowed connections, DHCP, and the networks in `VNET_LOCKDOWN_ALLOW` (comma-separated CIDRs, for the admins' SSH). Everything else is dropped in both directions and IPv6 is blocked. The rules follow the agent to a new switch and are removed when it stops. Contestants must not have root on the machine, or none of this holds.

## Limits and counters

Each agent may send `VNET_AGENT_PPS` packets per second (20000) and `VNET_AGENT_MBPS` megabits per second (200) into its switch, with a burst of one second's worth; 0 removes a limit. Packets beyond that are dropped and counted. A link whose peer stops reading drops data packets once 2 MB are waiting, and stays up.

Every `VNET_STATS_SEC` seconds (60; 0 off) a switch prints one line of counters: packets delivered, forwarded, held for a route, dropped by reason, link-full drops, peers, pending tokens, conductor state. `SIGUSR1` prints it at once together with one line per agent (packets and bytes in and out, drops). `VNET_DEBUG=1` adds a line per dropped or held packet.

## Liveness and recovery

Heartbeats are sent every 10 seconds. A peer with no received traffic for 30 seconds is closed. Override either with `VNET_HEARTBEAT_SEC` and `VNET_DEAD_SEC` on that process; the dead time must be larger than the heartbeat.

A switch that loses the conductor keeps forwarding, dials it again every 2 seconds, registers again and announces every agent it holds. A conductor that restarts therefore learns the routes back from the switches; agents whose control socket died reconnect it with an MRP once their switch has registered again, and new agents can join. An agent that loses its switch closes its control socket, asks the conductor for a switch and authenticates to the one it gets, keeping its TUN up meanwhile. A switch's peer links are withdrawn when the conductor says so and redialed once the routes come back.

The conductor writes its state to `/var/vnet-dump.bin` (`VNET_DUMP_PATH`) every `VNET_DUMP_SEC` seconds (60), on `SIGUSR1`, and on shutdown. `vnet-state [dump] [auth file]` prints it: switches, agents with their switch and join time, machines still joining, and, with the auth file, the expected machines that are not connected. A conductor restart prints how many switches and agents were in the previous file but rebuilds its state from the switches, not from the file.

## Generating the files

`tools/vnet-roster.py roster.txt out --conductor <ip>[:port] [--tls]` turns one roster into every file an event needs: `out/auth.txt` for the conductor, `out/switch.txt` for every switch, `out/machines/<name>.env` for each machine's systemd unit, and with `--tls` a CA and a certificate per machine in `out/certs/`. Keys are random and kept across reruns. The roster lists switches, agents with their virtual IP, groups, `internet=` prefixes and `lockdown`, and `allow` rules between groups; `tools/vnet-roster.py -h` shows the format.

## Running an event

Before: generate the files with `vnet-roster`, install `switch.txt`, `auth.txt`, the env file and the certificates under `/etc/vnet` on each machine (`ca.key` stays with you), start the units, and check `vnet-state dump auth.txt` until nothing is listed under "expected but not connected". Ping across two switches from a contestant machine, and from a locked-down machine confirm that its LAN gateway does not answer.

During: `vnet-state` says who is missing; `SIGUSR1` on a switch says what it dropped and why. To add a machine, append it to the roster, rerun `vnet-roster`, copy `auth.txt` to the conductor and `switch.txt` to every switch, restart the conductor (switches and agents reconnect on their own) and send `SIGHUP` to every switch. To remove a machine, remove its lines the same way; it is cut off at its next connection, or at once if you also stop its agent.

When a process dies, its unit restarts it; the others recover as described above. If a whole switch host is lost, its agents move to the remaining switches. If the conductor host is lost, the data plane keeps running; new joins and rejoins wait until a conductor with the same auth file is back at the same address.

## Larger layout

`docker/scale/run.sh` starts one conductor, 10 switches, and 100 agents on a private Docker network. `SWITCHES` and `AGENTS` override those counts. Generated files stay in `docker/scale/out/` and are not committed. `docker/certs.sh` makes the certificates `docker/docker-compose.yml` expects.

## Tests

`cmake --build build --target coverage` and `--target memcheck` run the unit tests, as the CI does. `sudo tests/integration/run.sh build` runs the conductor, two switches and three agents in network namespaces with TLS, and checks registration and ACL, forged messages from a raw client, conductor and switch restarts, a cut link, route restore, lockdown, config reload, the per-agent limit under a flood, and throughput.

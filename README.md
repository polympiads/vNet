# polympiads/vNet — Virtualized Network

**Documentation:** [vnet.polympiads.ch](https://vnet.polympiads.ch), with tutorials, guides for running an event, troubleshooting and the full reference. For anything else, email us at [polympiads@epfl.ch](mailto:polympiads@epfl.ch).

vNet is a virtual networking system built for competitive programming events. It creates an isolated overlay network where participants (agents) talk to each other using virtual IPs, no matter where they physically are. Each agent can only reach what the config explicitly allows.

vNet powers the networking infrastructure of olympiads in informatics at EPFL.

---

## Architecture

```
┌─────────────┐        ┌─────────────┐        ┌─────────────┐
│   Conductor │◄──────►│   Switch    │◄──────►│    Agent    │
│             │        │  (BlackBox) │        │  (TUN iface)│
└─────────────┘        └─────────────┘        └─────────────┘
  Control plane          Data plane             End node
```

- **Conductor** — manages connections, assigns agents to switches, broadcasts routes. Never forwards packets.
- **Switch** — agents connect here. Forwards packets locally, to peer switches, or to the internet, via the **BlackBox**.
- **Agent** — end node. Creates a TUN interface with its virtual IP. Applications see normal IPv4 traffic.
- **BlackBox** — packet engine: anti-spoofing, ACL checks, routing decisions.

---

## How it works

The agent registers with the conductor, gets assigned to a switch with a one-time token, authenticates, and receives its virtual IP. It creates a TUN interface and enters its event loop.

When an agent authenticates, its switch tells the conductor, which broadcasts the route to every other switch. Switches connect to each other on demand and cache routes locally — no per-packet conductor lookups. When a switch disconnects, the conductor tells everyone to drop routes pointing to it.

Once routes are known, packets flow switch-to-switch without the conductor involved. The BlackBox on each switch decides: deliver locally, forward to a peer switch, forward to the internet, or drop.

---

## Setup

Three processes. One conductor, one or more switches, and one agent per machine. Start them in any order: a switch or an agent that cannot reach the conductor keeps trying every 2 seconds, and an agent that loses its switch asks the conductor for another.

The conductor listens on `CONDUCTOR_PORT` (5000 when unset) and holds the switch config (`VNET_CONFIG_PATH`), which it sends to every switch. It has no arguments.

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

Set `VNET_AUTH_FILE` on the conductor. Each line is `switch <name> <key>` or `agent <name> <key>`. The name and key must match what the process was given. Blank lines and `#` comments are ignored. Without the file the conductor refuses to start, since it would accept any name and key; `VNET_ALLOW_INSECURE=1` overrides that for a lab. `SIGHUP` reloads the file (`systemctl reload vnet-conductor`): added machines can join at once, removed ones are refused when they next connect, and a file that does not parse leaves the old records in force.

Set `VNET_TLS=1` on the conductor, every switch, and every agent, or on none of them. Without it every process refuses to start, since keys and routes would cross the network in the clear; `VNET_ALLOW_INSECURE=1` overrides that too. The three variables `VNET_TLS_CERT`, `VNET_TLS_KEY`, and `VNET_TLS_CA` are required when TLS is on.

Every certificate names its role in the subject's `OU` field: `conductor`, `switch` or `agent`. Each side checks the role of the peer it talks to, so the files on a contestant machine cannot be used to run a switch or a conductor. Hostnames are not checked. `tools/vnet-certs.sh init <dir>` makes a CA, `tools/vnet-certs.sh issue <dir> <role> <name>` makes a signed certificate; keep `ca.key` off every machine. `VNET_TLS_ROLE_CHECK=0` accepts certificates without a role, for a deployment that still uses one certificate everywhere.

Beyond keys and certificates, the switch accepts a token, a route or a peer notice only from its conductor link; a switch accepts another switch as a peer only with the token the conductor issued for that pair; a connection that has not identified itself within 10 seconds is closed; a header that announces more than 65994 bytes closes the connection; a connection identifies itself once.

## Switch config

Set `VNET_CONFIG_PATH` on the conductor. Each switch applies the ACL itself and must know every virtual address, so the conductor sends the file to a switch when it registers, and to every switch after `SIGHUP` (`systemctl reload vnet-conductor`). A switch confirms with a fingerprint of the parsed content (comments and ordering do not matter), and the conductor names a switch whose fingerprint is not the one it sent. A file larger than one frame travels in pieces; the switch applies the whole or nothing. A switch started without a config of its own accepts no agent until the first one arrives, and keeps the last one it applied while the conductor is down.

A switch may still read a file of its own (`VNET_CONFIG_PATH` on the switch, `SIGHUP` reloads it), which a config from the conductor replaces. When the conductor has no `VNET_CONFIG_PATH`, that is the only source, and the conductor refuses a switch whose fingerprint differs from the registered switches', naming both; `VNET_CONFIG_HASH_CHECK=0` turns that off.

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

To change the rules, edit the file on the conductor and reload it; nothing restarts. Rules apply to the next packet; a changed or removed address takes effect when that agent next connects. A file that does not parse leaves the previous config in force, on the conductor and on every switch.

A switch announces itself as network `::internet` (`VNET_NETWORK`), so the conductor may assign any agent to it; an agent announces `vnet.internal` (`VNET_NETWORK`). The conductor assigns an agent to a switch of the same network or of `::internet`, round-robin across the switches that are up.

Allowed internet traffic is masqueraded out of the switch's physical default route. The switch TUN is `vnet-out` at `192.0.2.1/32`. `VNET_ADVERTISE_IP` is the address agents and peers dial when the one the conductor sees is not it (NAT, several interfaces).

## Routes that must stay off the TUN

After the TUN comes up, the agent pins the conductor address and the assigned switch address as `/32` routes via the previous physical gateway. Overlay addresses go through the TUN. Do not give the conductor or a switch an address inside `VNET_OVERLAY_CIDR`. When the agent stops it puts the default route back and removes the pins. The physical route is also kept in `/run/vnet/agent-<name>.route` (`VNET_STATE_DIR`), so an agent started after a crash puts it back too.

Anything else the machine must keep reaching on the physical network while the default route is the TUN goes in the bypass file, `VNET_BYPASS_FILE`:

```
# a VDI broker in another subnet, a DNS server
route 192.0.2.0/24
route 198.51.100.53
```

Each `route` prefix is pinned the same way, whether or not lockdown is on. A prefix inside a subnet the machine is directly on needs no pin and gets none; a prefix the machine already routes through another gateway is left to that route. A prefix that overlaps the overlay, or `0.0.0.0/0`, is refused. A machine on a directly connected subnet, such as the LAN gateway, is reachable without any of this. `VNET_LOCKDOWN_ALLOW` (comma-separated prefixes) means the same as `route` lines and still works.

## Lockdown of contestant machines

The overlay only governs what enters the TUN. Two contestants on the same LAN can still reach each other directly, and a dual-stack network leaves the internet open over IPv6. With `VNET_LOCKDOWN=1` the agent adds host firewall rules (chains `VNET_IN` and `VNET_OUT`, hooked first in `INPUT` and `OUTPUT`) that allow only loopback, the TUN, the links to the conductor and the switch, replies to allowed connections, DHCP, and what the bypass file allows. Everything else is dropped in both directions and IPv6 is blocked. The rules follow the agent to a new switch and are removed when it stops. Contestants must not have root on the machine, or none of this holds.

The bypass file carries the exceptions, so nothing about a particular site is in vNet. Its `route` prefixes are allowed in both directions on every port. Its `in` and `out` lines are iptables rules appended to `VNET_IN` and `VNET_OUT` before the final drop, exactly as written; `in6` and `out6` do the same for IPv6:

```
# the connection servers, both ways
route 192.0.2.10/32
route 192.0.2.11/32
# display protocol from the client subnet, inbound only
in  -p tcp -s 198.51.100.0/24 --dport 22443 -j ACCEPT
# one outbound service on the gateway
out -p tcp -d 203.0.113.1 --dport 4001:4002 -j ACCEPT
```

The arguments go to `iptables` without a shell, so a line cannot run anything; a line iptables refuses is logged and the drop stays. A file that cannot be opened stops the agent at start, so a wrong path cannot leave a machine open or a broker unreachable. `SIGHUP` (`systemctl reload vnet-agent`) reads the file again: routes that left it are unpinned, new ones pinned, the chains rebuilt.

`VNET_LOCKDOWN_LOG=1` logs each dropped packet to the kernel log (`journalctl -k`, prefixes `vnet-drop-in` and `vnet-drop-out`), one line per second at most. Turn it on for the first machine locked down on a new network: it lists every flow the image or its agents need that the bypass file does not allow yet. Without lockdown, only the `route` lines of the bypass file have an effect.

On a VDI, the in-guest agent of the broker (Horizon Agent, say) talks to its connection servers and receives the display sessions over the physical network, and the hypervisor heartbeat of VMware Tools does not use the network at all. Put the connection servers, the gateway appliance if sessions are tunneled, the DNS and NTP servers, and the domain controllers if the desktops are domain-joined, in `route` lines; with direct client connections add an `in` line for the display port from the client subnet. When the infrastructure already isolates the desktops from each other, `VNET_LOCKDOWN=0` and `VNET_DEFAULT_VIA_TUN=0` need no bypass file at all.

## Limits and counters

Each agent may send `VNET_AGENT_PPS` packets per second (20000) and `VNET_AGENT_MBPS` megabits per second (200) into its switch, with a burst of one second's worth; 0 removes a limit. Packets beyond that are dropped and counted. A link whose peer stops reading drops data packets once 2 MB are waiting, and stays up.

Every `VNET_STATS_SEC` seconds (60; 0 off) a switch prints one line of counters: packets delivered, forwarded, held for a route, dropped by reason, link-full drops, peers, pending tokens, conductor state. `SIGUSR1` prints it at once together with one line per agent (packets and bytes in and out, drops). `VNET_DEBUG=1` adds a line per dropped or held packet.

## Liveness and recovery

Heartbeats are sent every 10 seconds. A peer with no received traffic for 30 seconds is closed. Override either with `VNET_HEARTBEAT_SEC` and `VNET_DEAD_SEC` on that process; the dead time must be larger than the heartbeat.

A switch that loses the conductor keeps forwarding, dials it again every 2 seconds, registers again and announces every agent it holds. A conductor that restarts therefore learns the routes back from the switches; agents whose control socket died reconnect it with an MRP once their switch has registered again, and new agents can join. An agent that loses its switch closes its control socket, asks the conductor for a switch and authenticates to the one it gets, keeping its TUN up meanwhile. A switch's peer links are withdrawn when the conductor says so and redialed once the routes come back.

The conductor writes its state to `/var/vnet-dump.bin` (`VNET_DUMP_PATH`) every `VNET_DUMP_SEC` seconds (60), on `SIGUSR1`, and on shutdown. `vnet-state [dump] [auth file]` prints it: switches, agents with their switch and join time, machines still joining, and, with the auth file, the expected machines that are not connected. A conductor restart prints how many switches and agents were in the previous file but rebuilds its state from the switches, not from the file.

## Generating the files

`tools/vnet-roster.py roster.txt out --conductor <ip>[:port] [--tls]` turns one roster into every file an event needs: `out/auth.txt` and `out/switch.txt` for the conductor, `out/machines/<name>.env` for each machine's systemd unit, `out/machines/<name>.bypass` for each agent that has bypass lines, and with `--tls` a CA and a certificate per machine in `out/certs/`. Keys are random and kept across reruns. The roster lists switches, agents with their virtual IP, groups, `internet=` prefixes, `lockdown` and `droplog`, `allow` rules between groups, and `bypass` lines per group; `tools/vnet-roster.py -h` shows the format.

## Running an event

Before: generate the files with `vnet-roster`, install `auth.txt` and `switch.txt` on the conductor, and the env file, the bypass file if there is one, and the certificates under `/etc/vnet` on each machine (`ca.key` stays with you), start the units, and check `vnet-state dump auth.txt` until nothing is listed under "expected but not connected". Ping across two switches from a contestant machine, and from a locked-down machine confirm that its LAN gateway does not answer and that whatever its bypass file names still does.

During: `vnet-state` says who is missing; `SIGUSR1` on a switch says what it dropped and why. To add a machine or change a rule, edit the roster, rerun `vnet-roster`, copy `auth.txt` and `switch.txt` to the conductor, and `systemctl reload vnet-conductor`; every switch gets the new config and nothing restarts. To remove a machine, remove its lines the same way; it is cut off at its next connection, or at once if you also stop its agent.

When a process dies, its unit restarts it; the others recover as described above. If a whole switch host is lost, its agents move to the remaining switches. If the conductor host is lost, the data plane keeps running; new joins and rejoins wait until a conductor with the same auth file is back at the same address.

## Larger layout

`docker/scale/run.sh` starts one conductor, 10 switches, and 100 agents on a private Docker network. `SWITCHES` and `AGENTS` override those counts. Generated files stay in `docker/scale/out/` and are not committed. `docker/certs.sh` makes the certificates `docker/docker-compose.yml` expects.

## Known limits

The overlay rides TCP (with TLS) between agent and switch and between switches, so loss on the physical network is retried by both the inner and the outer connection; keep the physical path clean, and measure before an event that crosses the internet. Each switch is one thread; one machine saturating its switch is held back by the per-agent limit, not by more cores. The conductor is a single process: while it is down, machines already connected keep their data path and nothing can join.

## Tests

`cmake --build build --target coverage` and `--target memcheck` run the unit tests, as the CI does. `sudo tests/integration/run.sh build` runs the conductor, two switches and three agents in network namespaces with TLS, and checks registration and ACL, forged messages from a raw client, conductor and switch restarts, a cut link, route restore, lockdown, the bypass file, the config sent by the conductor and reloaded, the per-agent limit under a flood, and throughput.

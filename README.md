# polympiads/vNet - Virtualized Network

The purpose of the vNet software is to provide us with a way to link the virtual machines (both the ones used by our contestants and the ones used by host and runners), as part of a unified virtual network with security groups and network forwarding to the global internet.

## Setup

Three processes. One conductor, one or more switches, and one agent per machine. Start the conductor, then the switches, then the agents. An agent that starts before any switch is registered gives up after about 40 seconds.

The conductor listens on port 5000. It has no arguments.

```
conductor
```

A switch registers with the conductor, then listens for agents on the port you pass it.

```
switch <name> <auth_key> <port>
```

An agent asks the conductor for a switch, then opens a TUN named `vnet-<name>`. The name must be at most 15 characters, so the agent name itself must be at most 10.

```
agent <name> <auth_key>
```

`CONDUCTOR_IP` and `CONDUCTOR_PORT` are required on every switch and every agent. Use an address that is reachable on the physical network. Switches and agents also need `CAP_NET_ADMIN` and `/dev/net/tun`. A switch needs `net.ipv4.ip_forward=1` and `rp_filter=0` on its egress interface. In-container `sysctl -w` is not enough when `/proc/sys` is read-only; set the sysctls from outside.

## Auth and TLS

Set `VNET_AUTH_FILE` on the conductor or it accepts any name and key. Each line is `switch <name> <key>` or `agent <name> <key>`. The name and key must match the process arguments. Blank lines and `#` comments are ignored.

Set `VNET_TLS=1` on the conductor, every switch, and every agent, or on none of them. The three variables `VNET_TLS_CERT`, `VNET_TLS_KEY`, and `VNET_TLS_CA` are required when TLS is on. Use the same certificate as the certificate, the key, and the CA. The check is that the peer was signed by that CA. Hostnames are not checked.

## Switch config

`VNET_CONFIG_PATH` is required. The file is read once at startup. Copy the same file to every switch: each switch applies the ACL itself and must know every virtual address.

Two kinds of lines:

```
agent1  10.0.1.1
agent1  agent2
agent1  ::internet:8.8.8.8
```

The first assigns a virtual address inside the overlay (`10.0.0.0/8` unless `VNET_OVERLAY_CIDR` is set). Names and addresses must be unique.

`agent1 agent2` allows those two machines to talk. The rule is bidirectional.

`agent1 ::internet:8.8.8.8` allows that machine to exchange packets with that one internet address, through the switch. There is no rule for every internet host. Return traffic is accepted only for an address that was allowed outbound. If contestants should use their machine's normal route for general internet, set `VNET_DEFAULT_VIA_TUN=0` on the agent. Otherwise the agent replaces the default route with the TUN, and only listed internet addresses pass.

A switch announces itself as network `::internet`, so the conductor may assign any agent to it. Assignment is round-robin across the switches that are up.

Allowed internet traffic is masqueraded out of the switch's physical default route. The switch TUN is `vnet-out` at `192.0.2.1/32`.

## Routes that must stay off the TUN

After the TUN comes up, the agent pins the conductor address and the assigned switch address as `/32` routes via the previous physical gateway. Overlay addresses go through the TUN. Do not give the conductor or a switch an address inside `VNET_OVERLAY_CIDR`.

## Liveness

Heartbeats are sent every 30 seconds. A peer with no received traffic for 90 seconds is closed. Override either with `VNET_HEARTBEAT_SEC` and `VNET_DEAD_SEC` on that process.

The conductor writes its state to `/var/vnet-dump.bin` (`VNET_DUMP_PATH`) about once a minute, on `SIGUSR1`, and on shutdown. A restart prints how many switches and agents were in the previous file. It does not reopen those sessions. Machines that are already on a switch keep their data path. A conductor restart does not accept an agent rejoin until the switches have registered again.

## Larger layout

`docker/scale/run.sh` starts one conductor, 10 switches, and 100 agents on a private Docker network. `SWITCHES` and `AGENTS` override those counts. Generated files stay in `docker/scale/out/` and are not committed.

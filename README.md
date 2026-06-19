# polympiads/vNet — Virtualized Network

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

## Getting started

**Prerequisites:** Docker, Docker Compose, a Linux host.

```bash
git clone https://github.com/polympiads/vNet
cd vNet

mkdir -p config
echo "agent1  10.0.1.1" > config/switch1.txt
echo "agent1  10.0.1.1" > config/switch2.txt
echo "agent1  10.0.1.1" > config/switch3.txt

docker compose -f docker/docker-compose.yml up --build
```

Check it's working:

```bash
docker logs vnet-conductor
docker logs vnet-agent
docker exec vnet-agent ip addr show vnet-agent1
```

---

## Config file format

One file per switch, set via `VNET_CONFIG_PATH`.

```
# agent_name  virtual_ip
contestant1  10.0.1.1
gameserver   10.0.0.1

# ACL — agent-to-agent (bidirectional)
contestant1  gameserver

# ACL — internet access
gameserver   ::internet:8.8.8.8
```

`#` starts a comment. Unlisted agents can't authenticate. Agent-to-agent ACLs are bidirectional. Internet ACLs also allow return traffic.

---

## Docker Compose

Switches need `CONDUCTOR_IP`, `CONDUCTOR_PORT`, `VNET_CONFIG_PATH`, `NET_ADMIN`, and `/dev/net/tun`. Agents need `NET_ADMIN` and `/dev/net/tun` only — no config file.

---

## State dump

The conductor periodically writes its state (switches, agents, pending tokens) to a binary file, default `/var/vnet-dump.bin`, configurable via `VNET_DUMP_PATH`. Send `SIGUSR1` to the conductor process for an immediate dump. Schema is in `proto/state.proto`.

---

## Running tests

```bash
docker build -f docker/Dockerfile.test -t vnet-test .
docker run --rm vnet-test # all tests
docker run --rm vnet-test cmake --build . --target coverage
docker run --rm vnet-test cmake --build . --target memcheck
```

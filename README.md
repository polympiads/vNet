
# polympiads/vNet - Virtualized Network

vNet is a virtual networking system designed for competitive programming events. It creates an isolated overlay network where participants (agents) communicate using virtual IP addresses, regardless of their physical location. Each agent is isolated from the real network and can only reach the machines explicitly allowed by the configuration.

vNet was built to power the networking infrastructure of olympiads in informatics at EPFL.

---

## Architecture

vNet has three components:

```
┌─────────────┐        ┌─────────────┐        ┌─────────────┐
│   Conductor │◄──────►│   Switch    │◄──────►│    Agent    │
│             │        │  (BlackBox) │        │  (TUN iface)│
└─────────────┘        └─────────────┘        └─────────────┘
  Control plane          Data plane             End node
```

### Conductor
The control plane. Manages all connections, assigns agents to switches, and broadcasts routing information. Does not forward any packets - its only job is coordination.

### Switch
The data plane. Agents connect to a switch after the MIP handshake. The switch forwards IPv4 packets between local agents, to peer switches, or to the internet via a TUN device. Packet routing decisions are made by the **BlackBox**.

### Agent
An end node. After connecting, it creates a TUN network interface with its assigned virtual IP. From that point, any application on the agent machine sees normal IPv4 traffic through the virtual interface.

### BlackBox
The switch's packet processing engine. It handles:
- **Anti-spoofing** - verifies the source IP matches the agent's registered IP
- **ACL enforcement** - checks whether two agents are allowed to communicate
- **Routing decisions** - deliver locally, forward to peer switch, forward to internet, or drop

### NetQueue
An epoll-based I/O abstraction used by all three components. Handles non-blocking reads, a per-connection write buffer (64KB limit), and TUN device file descriptors.

---

## How it works

### 1. Connection setup (MIP - Machine Initialization Procedure)

```
Agent                    Conductor                      Switch
  │                          │                              │
  │── AGENT_MIP ────────────►│                              │
  │                          │── AGENT_CONNECTION_TOKEN ──► │
  │◄── CONNECT_TO_SWITCH ────│                              │
  │                          │                              │
  │── AUTH_CONNECT_TO_SWITCH ──────────────────────►        │
  │◄── CONNECTION_ACCEPTED (virtual_ipv4) ──────────        │
  │                          │                              │
  │  tun_open("vnet-<name>", virtual_ipv4)                  │

enters event loop                                   enters event loop
```

1. The agent connects to the conductor and sends its name and auth key
2. The conductor picks a switch using round-robin load balancing and generates a one-time token
3. The conductor sends the switch assignment to the agent and notifies the switch of the incoming agent
4. The agent connects to the switch and authenticates with the token
5. The switch validates the token, looks up the agent's virtual IP from the config file, and sends it back in `CONNECTION_ACCEPTED`
6. The agent creates a TUN interface with the assigned virtual IP and enters its event loop

### 2. Route broadcasting

When an agent authenticates with a switch, the switch notifies the conductor via `AGENT_REGISTERED`. The conductor then broadcasts a `SWITCH_ROUTE_UPDATE` to all other switches, telling them which switch handles that agent's virtual IP. Switches connect to each other on demand and cache routes locally - no per-packet route lookups go through the conductor.

When a switch disconnects, the conductor broadcasts `SWITCH_DISCONNECTED` and all other switches remove routes pointing to it.

### 3. Packet forwarding

Once connected, packets flow entirely through the switches without involving the conductor:

```
Agent A ──(PacketIPv4Raw)──► Switch A ──(PacketIPv4Raw)──► Switch B ──► Agent B
```

The BlackBox processes every packet and returns one of:
- `DELIVER_LOCAL` - destination is a local agent on this switch
- `FORWARD_SWITCH` - destination is on a peer switch
- `FORWARD_INTERNET` - destination is an internet IP (requires TUN on switch)
- `DROP` - bad packet, ACL denied, or unknown source

---

## Getting started

### Prerequisites

- Docker and Docker Compose
- A Linux host (vNet uses Linux-specific APIs: `epoll`, TUN devices, `accept4`)

### Build and run

```bash
# Clone the repository
git clone https://github.com/polympiads/vNet
cd vNet

# Create config files for each switch
mkdir -p config
echo "agent1  10.0.1.1" > config/switch1.txt
echo "agent1  10.0.1.1" > config/switch2.txt
echo "agent1  10.0.1.1" > config/switch3.txt

# Start the full stack
docker compose -f docker/docker-compose.yml up --build
```

### Check it works

```bash
docker logs vnet-conductor   # should show switches registering
docker logs vnet-switch3     # should show agent authenticating
docker logs vnet-agent       # should show TUN interface coming up

# Verify the TUN interface inside the agent container
docker exec vnet-agent ip addr show vnet-agent1
```

---

## Config file format

Each switch has its own config file, mounted via `VNET_CONFIG_PATH`. The file maps agent names to virtual IPs and defines ACL rules.

```
# IP assignments - agent_name  virtual_ip
contestant1  10.0.1.1
contestant2  10.0.1.2
gameserver   10.0.0.1

# ACL rules - agent-to-agent (bidirectional)
contestant1  gameserver
contestant2  gameserver

# ACL rules - internet access (specific IPs only)
gameserver   ::internet:8.8.8.8
gameserver   ::internet:1.1.1.1
```

Rules:
- Lines starting with `#` are comments
- IP assignment lines: second token is a valid IPv4 address
- ACL lines: second token is either another agent name or `::internet:ip`
- Agent-to-agent ACLs are bidirectional - `A  B` allows both A→B and B→A
- Internet ACLs allow both outbound traffic and return traffic from that IP
- Agents not listed in the config cannot authenticate with the switch

---

## Docker Compose

The default `docker/docker-compose.yml` runs one conductor, three switches, and one agent.

Each switch requires:
- `CONDUCTOR_IP` and `CONDUCTOR_PORT` environment variables
- `VNET_CONFIG_PATH` pointing to its config file (mounted as a volume)
- `NET_ADMIN` capability and `/dev/net/tun` device access

```yaml
switch1:
  environment:
    - CONDUCTOR_IP=vnet-conductor
    - CONDUCTOR_PORT=5000
    - VNET_CONFIG_PATH=/etc/vnet/config.txt
  volumes:
    - ../config/switch1.txt:/etc/vnet/config.txt:ro
  cap_add:
    - NET_ADMIN
  devices:
    - /dev/net/tun:/dev/net/tun
  command: ["/usr/local/bin/switch", "switch1", "authkey1", "6000"]
```

Agents only need `NET_ADMIN` and `/dev/net/tun` - no config file.

---

## Protocol messages

All messages are serialized with Protocol Buffers over TCP. The wire format is a 6-byte header (4-byte payload size + 2-byte packet type) followed by the protobuf payload.

| Message | Direction | Description |
|---------|-----------|-------------|
| `SWITCH_MIP` | Switch → Conductor | Switch registers with name, auth key, port |
| `AGENT_MIP` | Agent → Conductor | Agent registers with name and auth key |
| `CONNECT_TO_SWITCH` | Conductor → Agent | Switch assignment and one-time token |
| `AGENT_CONNECTION_TOKEN` | Conductor → Switch | Notify switch of incoming agent |
| `AUTH_CONNECT_TO_SWITCH` | Agent → Switch | Agent presents token to switch |
| `CONNECTION_ACCEPTED` | Switch → Agent | Authentication confirmed + virtual IP |
| `AGENT_REGISTERED` | Switch → Conductor | Notify conductor of agent's virtual IP |
| `SWITCH_ROUTE_UPDATE` | Conductor → Switch | Broadcast route for a new agent |
| `SWITCH_DISCONNECTED` | Conductor → Switch | Broadcast switch disconnection |
| `IPV4_RAW` | Switch ↔ Switch | Raw IPv4 packet forwarded between switches |
| `HEARTBEAT` | Any → Any | Keep-alive, no payload |

---

## State dump

The conductor periodically writes its full state to a binary file (default `/var/vnet-dump.bin`, configurable via `VNET_DUMP_PATH`). An on-demand dump can be triggered by sending `SIGUSR1` to the conductor process.

```bash
# Trigger an immediate dump
docker exec vnet-conductor kill -USR1 1

# Copy and read the dump
docker cp vnet-conductor:/var/vnet-dump.bin /tmp/vnet-dump.bin

# Generate Python bindings and read
python3 -m grpc_tools.protoc -I./proto --python_out=/tmp ./proto/state.proto
python3 -c "
import sys; sys.path.insert(0, '/tmp')
import state_pb2
state = state_pb2.ConductorState()
state.ParseFromString(open('/tmp/vnet-dump.bin', 'rb').read())
print('Switches:', [s.name for s in state.switches])
print('Agents:', [a.name for a in state.agents])
"
```

---

## Running tests

Tests run inside Docker using the `Dockerfile.test` image, which includes coverage instrumentation and Valgrind.

```bash
# Run all tests
docker build -f docker/Dockerfile.test -t vnet-test .
docker run --rm vnet-test

# Run with coverage report
docker run --rm vnet-test cmake --build . --target coverage

# Run with memory check
docker run --rm vnet-test cmake --build . --target memcheck
```

---

## Project structure

```
vNet/
├── proto/                  # Protobuf definitions
│   ├── mip.proto           # All packet messages
│   └── state.proto         # Conductor state dump schema
├── include/
│   ├── common/             # config.h, socket_utils.h, tun.h
│   └── vnet/
│       ├── blackbox/       # BlackBox, AgentRegistry, RoutingTable, Config
│       ├── netqueue/       # NetQueue, NetworkElement, FSM, handlers
│       └── protocol/       # PacketHeader, PacketType, Dispatch
├── src/
│   ├── common/             # socket_utils.cpp, tun.cpp
│   ├── blackbox/           # Packet processing engine
│   ├── netqueue/           # epoll event loop
│   ├── protocol/           # Header, types, dispatch
│   ├── conductor/          # Conductor main
│   ├── switch/             # Switch main
│   └── agent/              # Agent main
├── tests/                  # Google Test suite
├── config/                 # Per-switch config files
└── docker/                 # Dockerfiles and docker-compose
```


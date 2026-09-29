#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <csignal>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>

#include "mip.pb.h"
#include "common/config.h"
#include "common/socket_utils.h"
#include "vnet/netqueue/handler.hpp"
#include "vnet/protocol/dispatch.hpp"
#include "vnet/netqueue/netqueue.hpp"
#include "vnet/blackbox/blackbox.hpp"
#include "vnet/blackbox/config.hpp"
#include "vnet/protocol/types.hpp"

using namespace vnet::protocol;
using namespace vnet::netqueue;
using namespace vnet::blackbox;
using clk = std::chrono::steady_clock;

static const int                  LISTEN_BACKLOG      = 64;
static const std::chrono::seconds HEARTBEAT_INTERVAL  {30};
static const std::chrono::seconds TOKEN_EXPIRY        {60};
static const int                  MAX_CONNECT_RETRIES = 20;
static const int                  RETRY_DELAY_MS      = 2000;

static volatile sig_atomic_t g_running = 1;
static void on_signal(int) { g_running = 0; }

// ---------------------------------------------------------------------------
//  Connection metadata
// ---------------------------------------------------------------------------

enum class ConnRole { CONDUCTOR, AGENT_PENDING, AGENT_AUTHENTICATED, PEER_SWITCH };

struct ConnInfo {
    ConnRole    role;
    std::string name;
    int         fd = -1;
    clk::time_point last_hb_sent = clk::now();
};

// ---------------------------------------------------------------------------
//  Switch state
// ---------------------------------------------------------------------------

struct SwitchState {
    /* Pending tokens pushed by the conductor before the agent connects */
    struct PendingToken {
        std::string    agent_name;
        clk::time_point created;
    };
    std::unordered_map<uint64_t, PendingToken> pending_tokens;

    /* Authenticated agent connections */
    std::vector<ConnInfo*> agents;
    
    /* Peer switch connections - keyed by switch name */
    std::unordered_map<std::string, ConnInfo*> peer_switches;

    /* Conductor connection info */
    ConnInfo* conductor = nullptr;

    void expire_tokens() {
        auto now = clk::now();
        for (auto it = pending_tokens.begin(); it != pending_tokens.end(); ) {
            if (now - it->second.created > TOKEN_EXPIRY)
                it = pending_tokens.erase(it);
            else
                ++it;
        }
    }
};

static SwitchState g_state;
static BlackBox* g_blackbox = nullptr;

// ---------------------------------------------------------------------------
//  Packets waiting on a route the conductor has not pushed yet
// ---------------------------------------------------------------------------

static const size_t               PENDING_PER_DEST = 16;
static const std::chrono::seconds PENDING_TTL{5};

struct BufferedPacket {
    SourceType      source_type;
    int             source_fd;
    std::string     bytes;
    clk::time_point created;
};

static std::unordered_map<uint32_t, std::vector<BufferedPacket>> g_pending;

static const char* drop_reason_str(DropReason reason) {
    switch (reason) {
        case DropReason::MALFORMED_PACKET: return "malformed";
        case DropReason::SOURCE_SPOOF:     return "spoof";
        case DropReason::SOURCE_UNKNOWN:   return "unknown-source";
        case DropReason::DEST_UNREACHABLE: return "unreachable";
        case DropReason::ACL_DENIED:       return "acl";
        default:                           return "none";
    }
}

static bool send_ipv4(NetQueue* queue, int fd, const uint8_t* raw, size_t len) {
    mip::PacketIPv4Raw fwd;
    fwd.set_payload(raw, len);
    return queue->send(fd, PacketType::IPV4_RAW, fwd);
}

static void buffer_for_route(uint32_t dest_ipv4, SourceType source_type,
                             int source_fd, const uint8_t* raw, size_t len) {
    auto& q = g_pending[dest_ipv4];
    if (q.size() >= PENDING_PER_DEST) q.erase(q.begin());
    q.push_back({source_type, source_fd,
                 std::string(reinterpret_cast<const char*>(raw), len),
                 clk::now()});
    std::cout << "[Switch] Buffered packet for " << ipv4_to_string(dest_ipv4)
              << " (" << q.size() << " waiting)\n";
}

static void apply_decision(NetQueue* queue, const PacketDecision& decision,
                           const uint8_t* raw, size_t len,
                           SourceType source_type, int source_fd);

static void flush_pending(NetQueue* queue, uint32_t dest_ipv4) {
    auto it = g_pending.find(dest_ipv4);
    if (it == g_pending.end()) return;

    std::vector<BufferedPacket> waiting = std::move(it->second);
    g_pending.erase(it);

    for (auto& pkt : waiting) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(pkt.bytes.data());
        PacketDecision decision = g_blackbox->process(
            pkt.source_type, pkt.source_fd, raw, pkt.bytes.size());
        if (decision.action == PacketAction::REQUEST_ROUTE) {
            buffer_for_route(decision.dest_ipv4, pkt.source_type,
                             pkt.source_fd, raw, pkt.bytes.size());
            continue;
        }
        apply_decision(queue, decision, raw, pkt.bytes.size(),
                       pkt.source_type, pkt.source_fd);
    }
}

static void expire_pending() {
    auto now = clk::now();
    for (auto it = g_pending.begin(); it != g_pending.end(); ) {
        auto& q = it->second;
        q.erase(std::remove_if(q.begin(), q.end(), [&](const BufferedPacket& p) {
            return now - p.created > PENDING_TTL;
        }), q.end());
        if (q.empty()) it = g_pending.erase(it);
        else ++it;
    }
}

static void apply_decision(NetQueue* queue, const PacketDecision& decision,
                           const uint8_t* raw, size_t len,
                           SourceType source_type, int source_fd) {
    switch (decision.action) {
        case PacketAction::DELIVER_LOCAL:
        case PacketAction::FORWARD_SWITCH:
            if (!send_ipv4(queue, decision.target_fd, raw, len)) {
                std::cerr << "[Switch] Failed to forward packet to fd "
                          << decision.target_fd << "\n";
            }
            break;
        case PacketAction::FORWARD_INTERNET: {
            if (decision.target_fd < 0) break;
            ssize_t n = ::write(decision.target_fd, raw, len);
            if (n < 0 && errno != EINTR) perror("[Switch] tun write");
            break;
        }
        case PacketAction::REQUEST_ROUTE:
            buffer_for_route(decision.dest_ipv4, source_type, source_fd, raw, len);
            break;
        case PacketAction::DROP:
            std::cerr << "[Switch] Drop (" << drop_reason_str(decision.drop_reason)
                      << ")\n";
            break;
    }
}

// ---------------------------------------------------------------------------
//  Dispatch
// ---------------------------------------------------------------------------

struct SwitchDispatch : public Dispatch {
    NetQueue* queue = nullptr;

    void set_queue(NetQueue* q) {
        queue = q;
    }

    /*
     * Received from the conductor: a new agent is about to connect.
     * Store the token so we can validate it later.
     */
    void onAgentConnectionToken(socket_data data,
                                mip::PacketAgentConnectionToken& pkt) override {
        uint64_t token = pkt.connection_token();
        std::string agent = pkt.agent_name();

        g_state.pending_tokens[token] = { agent, clk::now() };

        std::cout << "[Switch] Expecting agent " << agent
                  << " (token=" << token << ")\n";
    }

    /*
    * An agent that has connected to us sends its token.
    * Validate and either accept or drop.
    */
    void onAuthConnectToSwitch(socket_data data,
                            mip::PacketAuthConnectToSwitch& pkt) override {
        auto*    info  = static_cast<ConnInfo*>(data.ptr_data);
        uint64_t token = pkt.connection_token();

        auto it = g_state.pending_tokens.find(token);
        if (it == g_state.pending_tokens.end()) {
            std::cerr << "[Switch] Rejected agent: invalid token "
                    << token << "\n";
            data.net_element->state = SCK_ERROR;
            return;
        }

        std::string agent_name = it->second.agent_name;
        g_state.pending_tokens.erase(it);

        // Register agent in BlackBox — looks up virtual IP from config
        if (!g_blackbox->on_agent_authenticated(agent_name, data.fd)) {
            std::cerr << "[Switch] Agent " << agent_name
                    << " not found in config, rejecting\n";
            data.net_element->state = SCK_ERROR;
            return;
        }

        // Get the virtual IP the BlackBox assigned
        AgentEntry* entry = g_blackbox->agents().find_by_name(agent_name);
        if (!entry) {
            std::cerr << "[Switch] Failed to get agent entry for "
                    << agent_name << "\n";
            data.net_element->state = SCK_ERROR;
            return;
        }

        info->role = ConnRole::AGENT_AUTHENTICATED;
        info->name = agent_name;
        g_state.agents.push_back(info);

        // Send acceptance with virtual IP so agent can set up its TUN
        mip::PacketConnectionAccepted ack;
        ack.set_virtual_ipv4(entry->virtual_ipv4);
        queue->send(data.fd, PacketType::CONNECTION_ACCEPTED, ack);

        mip::PacketAgentRegistered reg;
        reg.set_agent_name(agent_name);
        reg.set_virtual_ipv4(entry->virtual_ipv4);
        queue->send(g_state.conductor->fd, PacketType::AGENT_REGISTERED, reg);

        std::cout << "[Switch] Agent " << info->name
                << " authenticated (fd=" << data.fd
                << ", ip=" << ipv4_to_string(entry->virtual_ipv4) << ")\n";

        flush_pending(queue, entry->virtual_ipv4);
    }

    void onHeartbeat(socket_data) override {}

    void onClose(close_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info) return;

        if (info->role != ConnRole::AGENT_PENDING) {
            std::cout << "[Switch] Connection closed: " << info->name
                    << " (fd=" << data.fd << ")\n";
        }

        if (info->role == ConnRole::AGENT_AUTHENTICATED) {
            // Notify BlackBox so it cleans up the agent registry
            g_blackbox->on_agent_disconnected(data.fd);
            g_state.agents.erase(
                std::remove(g_state.agents.begin(), g_state.agents.end(), info),
                g_state.agents.end());
        }

        if (info->role == ConnRole::PEER_SWITCH) {
            g_blackbox->on_switch_disconnected(data.fd);
            g_state.peer_switches.erase(info->name);
        }

        if (info == g_state.conductor) {
            g_state.conductor = nullptr;
        }

        delete info;
    }

    void onSwitchRouteUpdate(socket_data data,
                            mip::PacketSwitchRouteUpdate& pkt) override {
        std::string sw_name  = pkt.switch_name();
        uint32_t    sw_ipv4  = pkt.switch_ipv4();
        uint32_t    sw_port  = pkt.switch_port();
        uint32_t    agent_ip = pkt.agent_ipv4();

        // Check if we already have a connection to this switch
        auto it = g_state.peer_switches.find(sw_name);
        if (it == g_state.peer_switches.end()) {
            // Connect to the peer switch
            MachineConfig sw_cfg {
                ipv4_to_string(sw_ipv4),
                static_cast<uint16_t>(sw_port)
            };

            int sw_fd = connect_to(sw_cfg);
            if (sw_fd < 0) {
                std::cerr << "[Switch] Failed to connect to peer switch "
                        << sw_name << "\n";
                return;
            }

            set_nonblocking(sw_fd);
            int flag = 1;
            setsockopt(sw_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            auto* info = new ConnInfo();
            info->role = ConnRole::PEER_SWITCH;
            info->name = sw_name;
            info->fd   = sw_fd;

            g_state.peer_switches[sw_name] = info;

            if (queue->put_sck(sw_fd, info) == nullptr) {
                std::cerr << "[Switch] Failed to add peer switch fd to queue\n";
                close(sw_fd);
                delete info;
                return;
            }

            std::cout << "[Switch] Connected to peer switch " << sw_name << "\n";

            // Update routing table
            g_blackbox->on_route_update(agent_ip, sw_fd);
        } else {
            // Already connected - just update the route
            g_blackbox->on_route_update(agent_ip, it->second->fd);
        }

        std::cout << "[Switch] Route update: " << ipv4_to_string(agent_ip)
                << " → " << sw_name << "\n";

        flush_pending(queue, agent_ip);
    }

    void onSwitchDisconnected(socket_data data,
                            mip::PacketSwitchDisconnected& pkt) override {
        std::string sw_name = pkt.switch_name();

        auto it = g_state.peer_switches.find(sw_name);
        if (it == g_state.peer_switches.end()) return;

        // Close the connection
        queue->close(it->second->fd);

        std::cout << "[Switch] Peer switch disconnected: " << sw_name << "\n";
    }

    void onIPv4Raw(socket_data data,
                    mip::PacketIPv4Raw& pkt) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info) return;

        const std::string& payload = pkt.payload();
        if (payload.empty()) return;

        /*
         * Authenticated agents are checked for spoofing and ACL.
         * Peer switches already did that. Inbound peer sockets are still
         * accepted as AGENT_PENDING until the switch-hello handshake lands;
         * those packets are switch traffic, not agent traffic.
         */
        SourceType source_type;
        if (info->role == ConnRole::AGENT_AUTHENTICATED) {
            source_type = SourceType::AGENT;
        } else if (info->role == ConnRole::PEER_SWITCH ||
                   info->role == ConnRole::AGENT_PENDING) {
            source_type = SourceType::SWITCH;
        } else {
            return;
        }

        const uint8_t* raw = reinterpret_cast<const uint8_t*>(payload.data());
        size_t         len = payload.size();

        PacketDecision decision = g_blackbox->process(source_type, data.fd, raw, len);
        apply_decision(queue, decision, raw, len, source_type, data.fd);
    }
};

// ---------------------------------------------------------------------------
//  Heartbeat sender
// ---------------------------------------------------------------------------

static void send_heartbeats(NetQueue& queue) {
    auto now = clk::now();
    auto try_hb = [&](ConnInfo* c) {
        if (c && c->fd >= 0 && now - c->last_hb_sent >= HEARTBEAT_INTERVAL) {
            queue.send_heartbeat(c->fd);
            c->last_hb_sent = now;
        }
    };

    try_hb(g_state.conductor);
    for (auto* a : g_state.agents) try_hb(a);
}

// ---------------------------------------------------------------------------
//  connect_to with retries (waits for the target to be up)
// ---------------------------------------------------------------------------

static int connect_with_retry(const MachineConfig& mc, const char* label) {
    for (int attempt = 1; attempt <= MAX_CONNECT_RETRIES; attempt++) {
        int fd = connect_to(mc);
        if (fd >= 0) return fd;

        std::cerr << "[Switch] " << label << " attempt " << attempt
                  << "/" << MAX_CONNECT_RETRIES << " failed, retrying...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS));
    }
    return -1;
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (argc != 4) {
        std::cerr << "Usage: ./switch <name> <auth_key> <port>\n";
        return 1;
    }

    std::string switch_name = argv[1];
    std::string auth_key    = argv[2];
    uint16_t    port        = static_cast<uint16_t>(std::stoi(argv[3]));

    // --- Read conductor address from environment ---
    const char* cdt_ip_env   = std::getenv("CONDUCTOR_IP");
    const char* cdt_port_env = std::getenv("CONDUCTOR_PORT");
    const char* config_path = std::getenv("VNET_CONFIG_PATH");
    if (!cdt_ip_env || !cdt_port_env) {
        std::cerr << "[Switch] CONDUCTOR_IP and CONDUCTOR_PORT must be set\n";
        return 1;
    }
    if (!config_path) {
        std::cerr << "[Switch] VNET_CONFIG_PATH must be set\n";
        return 1;
    }

    // --- Load config and initialize BlackBox ---
    vnet::blackbox::Config config;
    if (!config.load(config_path)) {
        std::cerr << "[Switch] Failed to load config: " << config_path << "\n";
        return 1;
    }

    BlackBox blackbox(config, -1);  // -1 = no internet TUN yet
    g_blackbox = &blackbox;

    std::cout << "[Switch] Loaded config from " << config_path << "\n";

    MachineConfig conductor_cfg {
        cdt_ip_env,
        static_cast<uint16_t>(std::stoi(cdt_port_env))
    };

    // --- 1. Connect to conductor (blocking) and send Switch MIP ---
    std::cout << "[Switch] Connecting to conductor at "
              << conductor_cfg.ip << ":" << conductor_cfg.port << " ...\n";

    int cdt_sock = connect_with_retry(conductor_cfg, "conductor");
    if (cdt_sock < 0) {
        std::cerr << "[Switch] Cannot reach conductor after "
                  << MAX_CONNECT_RETRIES << " attempts\n";
        return 1;
    }

    mip::PacketSwitchMIP mip_pkt;
    mip_pkt.set_name(switch_name);
    mip_pkt.set_auth_key(auth_key);
    mip_pkt.set_network("::internet");   // reachable from any network
    mip_pkt.set_port(port);

    if (!send_protobuf_packet(cdt_sock, PacketType::SWITCH_MIP, mip_pkt)) {
        std::cerr << "[Switch] Failed to send MIP\n";
        close(cdt_sock);
        return 1;
    }

    std::cout << "[Switch] Registered with conductor as " << switch_name
              << " on port " << port << "\n";

    // Switch the conductor fd to non-blocking for the event loop
    set_nonblocking(cdt_sock);

    int flag = 1;
    setsockopt(cdt_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // --- 2. Create listener for agents ---
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listener < 0) { perror("socket"); return 1; }

    int optval = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listener, LISTEN_BACKLOG) < 0) {
        perror("listen"); return 1;
    }

    std::cout << "[Switch] Listening for agents on port " << port << "\n";

    // --- 3. NetQueue setup ---
    SwitchDispatch dispatch;
    NetworkQueueHandler handler = makeNetworkQueueHandler(&dispatch);
    NetQueue queue(handler, /*epoll_timeout_ms=*/200);
    dispatch.set_queue(&queue);

    // Add conductor connection to the queue
    auto* cdt_info = new ConnInfo();
    cdt_info->role = ConnRole::CONDUCTOR;
    cdt_info->name = "conductor";
    cdt_info->fd   = cdt_sock;
    g_state.conductor = cdt_info;

    if (queue.put_sck(cdt_sock, cdt_info) == nullptr) {
        std::cerr << "[Switch] Failed to add conductor fd to queue\n";
        return 1;
    }

    // --- 4. Event loop ---
    while (g_running) {
        // Accept agents (non-blocking)
        while (true) {
            int agent_fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK);
            if (agent_fd < 0) break;

            setsockopt(agent_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            auto* info = new ConnInfo();
            info->role = ConnRole::AGENT_PENDING;
            info->fd   = agent_fd;

            if (queue.put_sck(agent_fd, info) == nullptr) {
                close(agent_fd);
                delete info;
            }
        }

        // Process events
        queue.wait_and_process();

        // Expire old tokens and packets that never learned a route
        g_state.expire_tokens();
        expire_pending();

        // Heartbeats
        send_heartbeats(queue);
    }

    close(listener);
    std::cout << "[Switch] Shutting down.\n";
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}

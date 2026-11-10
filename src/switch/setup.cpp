#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>

#include "mip.pb.h"
#include "common/config.h"
#include "common/socket_utils.h"
#include "common/sysutil.h"
#include "common/tun.h"
#include <cstdlib>
#include "vnet/netqueue/handler.hpp"
#include "vnet/protocol/dispatch.hpp"
#include "vnet/netqueue/netqueue.hpp"
#include "vnet/blackbox/blackbox.hpp"
#include "vnet/blackbox/config.hpp"
#include "vnet/protocol/types.hpp"
#include "vnet/tls/session.hpp"

using namespace vnet::protocol;
using namespace vnet::netqueue;
using namespace vnet::blackbox;
using clk = std::chrono::steady_clock;

static const int                  LISTEN_BACKLOG      = 1024;
/* Handshakes run inside the queue, so a turn can take a whole burst. */
static const int                  ACCEPTS_PER_TURN    = 64;
/* A dialed peer that has not finished connecting and handshaking. */
static const std::chrono::seconds DIAL_TIMEOUT        {5};
/* A dead peer is noticed within g_dead; both can be set per process. */
static std::chrono::seconds       g_heartbeat{10};
static std::chrono::seconds       g_dead{30};
static const std::chrono::seconds IDENTIFY_TIMEOUT    {10};
static const std::chrono::seconds TOKEN_EXPIRY        {60};
/* Time between attempts to reach the conductor when the link is down. */
static const std::chrono::seconds CONDUCTOR_RETRY     {2};

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_request = 0;
static void on_signal(int) { g_running = 0; }
static void on_sighup(int) { g_reload_request = 1; }

// ---------------------------------------------------------------------------
//  Connection metadata
// ---------------------------------------------------------------------------

enum class ConnRole { CONDUCTOR, AGENT_PENDING, AGENT_AUTHENTICATED, PEER_SWITCH };

struct ConnInfo {
    ConnRole    role;
    std::string name;
    int         fd = -1;
    clk::time_point last_hb_sent = clk::now();
    clk::time_point last_rx = clk::now();
    clk::time_point accepted_at = clk::now();
    /* Retiring a duplicate dial must not wipe routes that were moved. */
    bool        skip_route_cleanup = false;
    /* Peer links: this side dialed, and whether the link is up yet. */
    bool        dialed    = false;
    bool        connected = true;
    clk::time_point dialed_at{};
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
static std::string g_overlay_cidr = "10.0.0.0/8";

/* What this switch tells the conductor about itself, kept for re-registration. */
struct SwitchIdentity {
    std::string   name;
    std::string   auth_key;
    std::string   network = "::internet";
    uint16_t      port = 0;
    MachineConfig conductor{"", 0};
    std::string   config_path;
    std::string   config_hash;
    uint32_t      advertise_ipv4 = 0;
};
static SwitchIdentity   g_identity;
static clk::time_point  g_last_conductor_dial{};

/* Accepted sockets that may still be waiting to authenticate or say hello. */
static std::unordered_set<ConnInfo*> g_unidentified;
static BlackBox* g_blackbox = nullptr;
static std::string g_switch_name;

struct PeerTarget {
    std::string     name;
    uint32_t        ipv4 = 0;
    uint16_t        port = 0;
    clk::time_point last_attempt{};
};

static std::unordered_map<std::string, PeerTarget> g_peer_targets;
/* Per peer switch, the token the conductor issued for that pair. */
static std::unordered_map<std::string, uint64_t>   g_peer_tokens;
static std::unordered_map<std::string, std::vector<uint32_t>> g_routes_waiting;

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

/*
 * Per-packet events are counted, not logged: a port scan from one
 * contestant must not fill the disk or slow the loop. VNET_DEBUG=1 turns
 * the per-packet lines on. Counters are printed every VNET_STATS_SEC and
 * on SIGUSR1, which also lists every agent.
 */
static bool     g_debug = false;
static uint64_t g_send_full_drops = 0;
static uint64_t g_tun_write_errors = 0;
static volatile sig_atomic_t g_stats_request = 0;
static void on_sigusr1(int) { g_stats_request = 1; }

static bool send_ipv4(NetQueue* queue, int fd, const uint8_t* raw, size_t len) {
    return queue->send_data(fd, PacketType::IPV4_RAW, raw, len);
}

static void buffer_for_route(uint32_t dest_ipv4, SourceType source_type,
                             int source_fd, const uint8_t* raw, size_t len) {
    auto& q = g_pending[dest_ipv4];
    if (q.size() >= PENDING_PER_DEST) q.erase(q.begin());
    q.push_back({source_type, source_fd,
                 std::string(reinterpret_cast<const char*>(raw), len),
                 clk::now()});
    if (g_debug)
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
                g_send_full_drops++;
                if (g_debug)
                    std::cerr << "[Switch] Link to fd " << decision.target_fd
                              << " is full; packet dropped\n";
            }
            break;
        case PacketAction::FORWARD_INTERNET: {
            if (decision.target_fd < 0) break;
            ssize_t n = ::write(decision.target_fd, raw, len);
            if (n != static_cast<ssize_t>(len)) {
                g_tun_write_errors++;
                if (g_debug) perror("[Switch] tun write");
            }
            break;
        }
        case PacketAction::REQUEST_ROUTE:
            buffer_for_route(decision.dest_ipv4, source_type, source_fd, raw, len);
            break;
        case PacketAction::DROP:
            if (g_debug)
                std::cerr << "[Switch] Drop (" << drop_reason_name(decision.drop_reason)
                          << ")\n";
            break;
    }
}

static void install_waiting_routes(NetQueue* queue, const std::string& name, int fd) {
    auto it = g_routes_waiting.find(name);
    if (it == g_routes_waiting.end()) return;
    for (uint32_t ip : it->second) {
        g_blackbox->on_route_update(ip, fd);
        flush_pending(queue, ip);
    }
    g_routes_waiting.erase(it);
}

/*
 * Start a connection to a peer switch. The connect and the TLS handshake
 * finish inside the queue; onConnected sends the hello. Nothing here
 * waits, so a peer that is down costs this switch no forwarding time.
 */
static int dial_peer(NetQueue* queue, const PeerTarget& target) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target.port);
    addr.sin_addr.s_addr = target.ipv4;

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd);
        std::cerr << "[Switch] Failed to connect to peer " << target.name << "\n";
        return -1;
    }

    auto* info = new ConnInfo();
    info->role      = ConnRole::PEER_SWITCH;
    info->name      = target.name;
    info->fd        = fd;
    info->dialed    = true;
    info->connected = false;
    info->dialed_at = clk::now();

    ssl_st* ssl = vnet::tls::enabled() ? vnet::tls::new_session(fd) : nullptr;
    if (vnet::tls::enabled() && !ssl) {
        ::close(fd);
        delete info;
        return -1;
    }
    if (queue->put_sck_connecting(fd, info, ssl) == nullptr) {
        vnet::tls::free_session(ssl);
        ::close(fd);
        delete info;
        return -1;
    }
    g_state.peer_switches[target.name] = info;

    std::cout << "[Switch] Dialing peer " << target.name << "\n";
    return fd;
}

static void retry_peers(NetQueue& queue) {
    auto now = clk::now();
    for (auto& entry : g_peer_targets) {
        PeerTarget& target = entry.second;
        if (g_state.peer_switches.count(target.name)) continue;
        // One side dials. The other accepts on the agent listener. If both
        // block in SSL_connect, neither ever accept()s.
        if (!(g_switch_name < target.name)) continue;
        // Nothing to show the peer yet; the conductor's token comes
        // right after its registration.
        if (!g_peer_tokens.count(target.name)) continue;
        if (now - target.last_attempt < std::chrono::seconds(2)) continue;
        target.last_attempt = now;
        dial_peer(&queue, target);
        return;
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
     * Tokens, route updates and route withdrawals are trusted only from
     * the conductor. Any other socket that sends one is closed, or an
     * agent could forge a token for another machine's name and log in as
     * it.
     */
    bool from_conductor(const socket_data& data, const char* what) {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (info && info->role == ConnRole::CONDUCTOR) return true;
        std::cerr << "[Switch] Dropped " << what
                  << " from a socket that is not the conductor (fd="
                  << data.fd << ")\n";
        data.net_element->state = SCK_ERROR;
        return false;
    }

    void onActivity(socket_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (info) info->last_rx = clk::now();
    }

    /* With TLS on, the certificate must name the role the peer claims. */
    bool cert_allows(socket_data data, const char* role, const std::string& who) {
        if (vnet::tls::peer_has_role(data.net_element->ssl, role)) return true;
        std::cerr << "[Switch] Rejected " << who << " (fd=" << data.fd
                  << "): certificate role is '"
                  << vnet::tls::peer_role(data.net_element->ssl) << "', expected "
                  << role << "\n";
        data.net_element->state = SCK_ERROR;
        return false;
    }

    void onConnected(socket_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info) return;
        info->connected = true;
        info->last_rx   = clk::now();

        if (info->role == ConnRole::PEER_SWITCH &&
            !cert_allows(data, vnet::tls::ROLE_SWITCH, "peer " + info->name)) {
            queue->close(data.fd);
            return;
        }
        if (info->role == ConnRole::CONDUCTOR &&
            !cert_allows(data, vnet::tls::ROLE_CONDUCTOR, "conductor")) {
            queue->close(data.fd);
            return;
        }

        if (info->role == ConnRole::PEER_SWITCH && info->dialed) {
            mip::PacketSwitchHello hello;
            hello.set_switch_name(g_switch_name);
            auto token = g_peer_tokens.find(info->name);
            hello.set_peer_token(token == g_peer_tokens.end() ? 0 : token->second);
            queue->send(data.fd, PacketType::SWITCH_HELLO, hello);
            std::cout << "[Switch] Peer " << info->name << " link up (dialed)\n";
            install_waiting_routes(queue, info->name, data.fd);
        }

        if (info->role == ConnRole::CONDUCTOR) {
            mip::PacketSwitchMIP mip_pkt;
            mip_pkt.set_name(g_identity.name);
            mip_pkt.set_auth_key(g_identity.auth_key);
            mip_pkt.set_network(g_identity.network);
            mip_pkt.set_port(g_identity.port);
            mip_pkt.set_config_hash(g_identity.config_hash);
            mip_pkt.set_advertise_ipv4(g_identity.advertise_ipv4);
            if (!queue->send(data.fd, PacketType::SWITCH_MIP, mip_pkt)) {
                std::cerr << "[Switch] Failed to send MIP\n";
                return;
            }
            std::cout << "[Switch] Registered with conductor as " << g_identity.name
                      << " on port " << g_identity.port << "\n";

            // A conductor that restarted knows nothing: tell it who is
            // here, so it can publish the routes again.
            size_t announced = 0;
            for (ConnInfo* agent : g_state.agents) {
                AgentEntry* entry = g_blackbox->agents().find_by_fd(agent->fd);
                if (!entry) continue;
                mip::PacketAgentRegistered reg;
                reg.set_agent_name(entry->name);
                reg.set_virtual_ipv4(entry->virtual_ipv4);
                queue->send(data.fd, PacketType::AGENT_REGISTERED, reg);
                announced++;
            }
            if (announced > 0)
                std::cout << "[Switch] Re-announced " << announced << " agent(s)\n";
        }
    }

    /*
     * Received from the conductor: a new agent is about to connect.
     * Store the token so we can validate it later.
     */
    void onAgentConnectionToken(socket_data data,
                                mip::PacketAgentConnectionToken& pkt) override {
        if (!from_conductor(data, "agent token")) return;
        uint64_t token = pkt.connection_token();
        std::string agent = pkt.agent_name();

        g_state.pending_tokens[token] = { agent, clk::now() };

        std::cout << "[Switch] Expecting agent " << agent << "\n";
    }

    /*
    * An agent that has connected to us sends its token.
    * Validate and either accept or drop.
    */
    void onAuthConnectToSwitch(socket_data data,
                            mip::PacketAuthConnectToSwitch& pkt) override {
        auto*    info  = static_cast<ConnInfo*>(data.ptr_data);
        uint64_t token = pkt.connection_token();

        if (!cert_allows(data, vnet::tls::ROLE_AGENT, "agent")) return;

        auto it = g_state.pending_tokens.find(token);
        if (it == g_state.pending_tokens.end()) {
            std::cerr << "[Switch] Rejected agent: unknown token (fd="
                    << data.fd << ")\n";
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

        // The same agent on an older socket: it rejoined before the old
        // link timed out. The registry already points at the new fd, so
        // closing the old one withdraws nothing.
        for (ConnInfo* old : g_state.agents) {
            if (old->name == agent_name && old != info) {
                std::cout << "[Switch] Agent " << agent_name
                          << " authenticated again; closing fd " << old->fd << "\n";
                queue->close(old->fd);
                break;
            }
        }

        info->role = ConnRole::AGENT_AUTHENTICATED;
        info->name = agent_name;
        g_state.agents.push_back(info);

        // Send acceptance with virtual IP so agent can set up its TUN
        mip::PacketConnectionAccepted ack;
        ack.set_virtual_ipv4(entry->virtual_ipv4);
        queue->send(data.fd, PacketType::CONNECTION_ACCEPTED, ack);

        // The token came from the conductor before it went away, so the
        // agent is still accepted for local traffic; only the route
        // announcement is skipped.
        if (g_state.conductor) {
            mip::PacketAgentRegistered reg;
            reg.set_agent_name(agent_name);
            reg.set_virtual_ipv4(entry->virtual_ipv4);
            queue->send(g_state.conductor->fd, PacketType::AGENT_REGISTERED, reg);
        } else {
            std::cerr << "[Switch] No conductor link; route for " << agent_name
                      << " was not announced\n";
        }

        std::cout << "[Switch] Agent " << info->name
                << " authenticated (fd=" << data.fd
                << ", ip=" << ipv4_to_string(entry->virtual_ipv4) << ")\n";

        flush_pending(queue, entry->virtual_ipv4);
    }

    void onHeartbeat(socket_data) override {}

    void onTunReady(tun_data data) override {
        if (!g_blackbox || data.ip_buffer == nullptr || data.ip_buffer_size < 20)
            return;
        if ((data.ip_buffer[0] >> 4) != 4) return;

        PacketDecision decision = g_blackbox->process(
            SourceType::TUN, data.fd, data.ip_buffer, data.ip_buffer_size);
        apply_decision(queue, decision, data.ip_buffer, data.ip_buffer_size,
                       SourceType::TUN, data.fd);
    }

    void onClose(close_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info) return;

        if (info->role != ConnRole::AGENT_PENDING) {
            std::cout << "[Switch] Connection closed: " << info->name
                    << " (fd=" << data.fd << ")\n";
        }

        if (info->role == ConnRole::AGENT_AUTHENTICATED) {
            AgentEntry* entry = g_blackbox->agents().find_by_fd(data.fd);
            if (entry && g_state.conductor && g_state.conductor->fd >= 0) {
                mip::PacketAgentUnregistered gone;
                gone.set_agent_name(entry->name);
                gone.set_virtual_ipv4(entry->virtual_ipv4);
                gone.set_switch_name(g_switch_name);
                queue->send(g_state.conductor->fd, PacketType::AGENT_UNREGISTERED, gone);
            }
            g_blackbox->on_agent_disconnected(data.fd);
            g_state.agents.erase(
                std::remove(g_state.agents.begin(), g_state.agents.end(), info),
                g_state.agents.end());
        }

        if (info->role == ConnRole::PEER_SWITCH) {
            auto it = g_state.peer_switches.find(info->name);
            if (it != g_state.peer_switches.end() && it->second == info)
                g_state.peer_switches.erase(it);
            if (!info->skip_route_cleanup)
                g_blackbox->on_switch_disconnected(data.fd);
        }

        if (info == g_state.conductor) {
            g_state.conductor = nullptr;
            std::cerr << "[Switch] Conductor link lost; agents keep their data "
                      << "path, retrying every " << CONDUCTOR_RETRY.count() << "s\n";
        }

        g_unidentified.erase(info);
        delete info;
    }

    void onSwitchHello(socket_data data, mip::PacketSwitchHello& pkt) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info || info->role == ConnRole::AGENT_AUTHENTICATED) return;
        if (info->role == ConnRole::CONDUCTOR) return;

        std::string remote = pkt.switch_name();
        if (remote.empty() || remote == g_switch_name) {
            data.net_element->state = SCK_ERROR;
            return;
        }
        if (!cert_allows(data, vnet::tls::ROLE_SWITCH, "peer hello from " + remote)) return;

        // Only a switch the conductor paired us with, showing that pair's
        // token, becomes a peer. Anything else on the agent port that
        // says hello is an agent trying to bypass the ACL.
        auto token = g_peer_tokens.find(remote);
        if (token == g_peer_tokens.end() || pkt.peer_token() == 0 ||
            pkt.peer_token() != token->second) {
            std::cerr << "[Switch] Rejected peer hello from '" << remote
                      << "' (fd=" << data.fd << "): "
                      << (token == g_peer_tokens.end() ? "no token for that switch yet"
                                                       : "wrong token") << "\n";
            data.net_element->state = SCK_ERROR;
            return;
        }

        auto existing = g_state.peer_switches.find(remote);
        bool have = existing != g_state.peer_switches.end() && existing->second != info;

        if (!have) {
            info->role = ConnRole::PEER_SWITCH;
            info->name = remote;
            g_state.peer_switches[remote] = info;
            std::cout << "[Switch] Peer " << remote << " connected inbound\n";
            install_waiting_routes(queue, remote, data.fd);
            return;
        }

        /* Both sides dialed. Keep the socket opened by the smaller name. */
        if (g_switch_name < remote) {
            data.net_element->state = SCK_ERROR;
            return;
        }

        ConnInfo* outbound = existing->second;
        int old_fd = outbound->fd;
        g_blackbox->retarget_switch(old_fd, data.fd);
        outbound->skip_route_cleanup = true;
        g_state.peer_switches.erase(existing);

        info->role = ConnRole::PEER_SWITCH;
        info->name = remote;
        g_state.peer_switches[remote] = info;
        queue->close(old_fd);

        std::cout << "[Switch] Collapsed duplicate link to " << remote << "\n";
        install_waiting_routes(queue, remote, data.fd);
    }

    void onPeerToken(socket_data data, mip::PacketPeerToken& pkt) override {
        if (!from_conductor(data, "peer token")) return;
        if (pkt.peer_name().empty() || pkt.token() == 0) return;
        g_peer_tokens[pkt.peer_name()] = pkt.token();
    }

    void onAgentUnregistered(socket_data data,
                             mip::PacketAgentUnregistered& pkt) override {
        if (!from_conductor(data, "route withdrawal")) return;
        g_blackbox->on_route_update(pkt.virtual_ipv4(), -1);
        std::cout << "[Switch] Route withdrawn: "
                  << ipv4_to_string(pkt.virtual_ipv4()) << "\n";
    }

    void onSwitchRouteUpdate(socket_data data,
                            mip::PacketSwitchRouteUpdate& pkt) override {
        if (!from_conductor(data, "route update")) return;
        std::string sw_name  = pkt.switch_name();
        uint32_t    sw_ipv4  = pkt.switch_ipv4();
        uint32_t    sw_port  = pkt.switch_port();
        uint32_t    agent_ip = pkt.agent_ipv4();

        if (sw_name == g_switch_name) return;

        auto known = g_peer_targets.find(sw_name);
        if (known == g_peer_targets.end()) {
            g_peer_targets.emplace(sw_name, PeerTarget{
                sw_name, sw_ipv4, static_cast<uint16_t>(sw_port), clk::time_point{}
            });
        } else {
            known->second.ipv4 = sw_ipv4;
            known->second.port = static_cast<uint16_t>(sw_port);
        }

        auto it = g_state.peer_switches.find(sw_name);
        if (it != g_state.peer_switches.end()) {
            g_blackbox->on_route_update(agent_ip, it->second->fd);
        } else {
            g_routes_waiting[sw_name].push_back(agent_ip);
        }

        std::cout << "[Switch] Route update: " << ipv4_to_string(agent_ip)
                << " → " << sw_name << "\n";

        flush_pending(queue, agent_ip);
    }

    void onSwitchDisconnected(socket_data data,
                            mip::PacketSwitchDisconnected& pkt) override {
        if (!from_conductor(data, "switch disconnect")) return;
        std::string sw_name = pkt.switch_name();
        g_peer_targets.erase(sw_name);
        g_routes_waiting.erase(sw_name);
        g_peer_tokens.erase(sw_name);

        auto it = g_state.peer_switches.find(sw_name);
        if (it == g_state.peer_switches.end()) return;

        int fd = it->second->fd;
        std::cout << "[Switch] Peer switch disconnected: " << sw_name << "\n";
        queue->close(fd);
    }

    void onIPv4Raw(socket_data data, const uint8_t* raw, size_t len) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info) return;

        SourceType source_type;
        if (info->role == ConnRole::AGENT_AUTHENTICATED) {
            source_type = SourceType::AGENT;
        } else if (info->role == ConnRole::PEER_SWITCH) {
            source_type = SourceType::SWITCH;
        } else {
            return;
        }

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
        if (c && c->fd >= 0 && now - c->last_hb_sent >= g_heartbeat) {
            queue.send_heartbeat(c->fd);
            c->last_hb_sent = now;
        }
    };

    if (g_state.conductor && g_state.conductor->connected) try_hb(g_state.conductor);
    for (auto* a : g_state.agents) try_hb(a);
    for (auto& peer : g_state.peer_switches)
        if (peer.second->connected) try_hb(peer.second);
}

static void reap_dead(NetQueue& queue) {
    auto now = clk::now();
    std::vector<int> dead;
    auto check = [&](ConnInfo* c) {
        if (c && c->fd >= 0 && now - c->last_rx >= g_dead)
            dead.push_back(c->fd);
    };
    if (g_state.conductor && !g_state.conductor->connected) {
        if (now - g_state.conductor->dialed_at >= DIAL_TIMEOUT) {
            std::cerr << "[Switch] Conductor did not answer the dial\n";
            dead.push_back(g_state.conductor->fd);
        }
    } else {
        check(g_state.conductor);
    }
    for (auto* agent : g_state.agents) check(agent);
    for (auto& peer : g_state.peer_switches) {
        ConnInfo* c = peer.second;
        if (!c->connected) {
            if (now - c->dialed_at >= DIAL_TIMEOUT) {
                std::cerr << "[Switch] Peer " << c->name
                          << " did not answer the dial\n";
                dead.push_back(c->fd);
            }
            continue;
        }
        check(c);
    }
    for (int fd : dead) {
        std::cerr << "[Switch] Heartbeat timeout on fd " << fd << "\n";
        queue.close(fd);
    }

    // A socket that never authenticates holds an fd and is never
    // heartbeat-checked, since it is in none of the lists above.
    std::vector<int> silent;
    for (auto it = g_unidentified.begin(); it != g_unidentified.end(); ) {
        ConnInfo* c = *it;
        if (c->role != ConnRole::AGENT_PENDING) {
            it = g_unidentified.erase(it);
            continue;
        }
        if (now - c->accepted_at >= IDENTIFY_TIMEOUT)
            silent.push_back(c->fd);
        ++it;
    }
    for (int fd : silent) {
        std::cerr << "[Switch] No authentication within "
                  << IDENTIFY_TIMEOUT.count() << "s on fd " << fd << "\n";
        queue.close(fd);
    }
}

// ---------------------------------------------------------------------------
//  connect_to with retries (waits for the target to be up)
// ---------------------------------------------------------------------------

/*
 * Packets written to the TUN are forwarded by the kernel and masqueraded
 * out the physical interface. Replies are routed back into the TUN, which
 * the event loop reads as SourceType::TUN.
 */
static bool setup_internet(const std::string& tun_name) {
    std::string egress;
    uint32_t gateway = 0;
    if (!read_default_route(egress, gateway)) {
        std::cerr << "[Switch] No default route for NAT\n";
        return false;
    }
    if (!valid_name(egress, 15)) {
        std::cerr << "[Switch] Unexpected egress interface name '" << egress << "'\n";
        return false;
    }

    const char* tag = "[Switch]";
    run_program({"sysctl", "-w", "net.ipv4.ip_forward=1"}, tag);
    run_program({"sysctl", "-w", "net.ipv4.conf.all.rp_filter=0"}, tag);
    run_program({"sysctl", "-w", "net.ipv4.conf." + tun_name + ".rp_filter=0"}, tag);
    run_program({"ip", "route", "replace", g_overlay_cidr, "dev", tun_name}, tag);

    // Each rule is added once: check first, append when the check fails.
    auto ensure = [&](const std::string& table, std::vector<std::string> rule) {
        std::vector<std::string> check = {"iptables"};
        std::vector<std::string> add   = {"iptables"};
        if (!table.empty()) {
            check.insert(check.end(), {"-t", table});
            add.insert(add.end(), {"-t", table});
        }
        check.push_back("-C");
        add.push_back("-A");
        check.insert(check.end(), rule.begin(), rule.end());
        add.insert(add.end(), rule.begin(), rule.end());
        return run_program(check, tag, /*quiet=*/true) || run_program(add, tag);
    };
    bool ok = ensure("nat", {"POSTROUTING", "-o", egress, "-j", "MASQUERADE"})
           && ensure("", {"FORWARD", "-i", tun_name, "-o", egress, "-j", "ACCEPT"})
           && ensure("", {"FORWARD", "-i", egress, "-o", tun_name, "-m", "conntrack",
                          "--ctstate", "RELATED,ESTABLISHED", "-j", "ACCEPT"});
    if (ok)
        std::cout << "[Switch] NAT " << g_overlay_cidr << " via " << tun_name
                  << " -> " << egress << "\n";
    return ok;
}

/*
 * Start a connection to the conductor. The connect and handshake finish
 * inside the queue; onConnected sends the MIP and re-announces the agents
 * already on this switch. Called at startup and whenever the link is
 * down, so a conductor restart needs no action here.
 */
static void dial_conductor(NetQueue& queue) {
    g_last_conductor_dial = clk::now();

    int fd = connect_start(g_identity.conductor.ip, g_identity.conductor.port);
    if (fd < 0) {
        std::cerr << "[Switch] Cannot start a connection to the conductor at "
                  << g_identity.conductor.ip << ":" << g_identity.conductor.port << "\n";
        return;
    }

    auto* info = new ConnInfo();
    info->role      = ConnRole::CONDUCTOR;
    info->name      = "conductor";
    info->fd        = fd;
    info->dialed    = true;
    info->connected = false;
    info->dialed_at = clk::now();

    ssl_st* ssl = vnet::tls::enabled() ? vnet::tls::new_session(fd) : nullptr;
    if (vnet::tls::enabled() && !ssl) {
        ::close(fd);
        delete info;
        return;
    }
    if (queue.put_sck_connecting(fd, info, ssl) == nullptr) {
        vnet::tls::free_session(ssl);
        ::close(fd);
        delete info;
        return;
    }
    g_state.conductor = info;
    std::cout << "[Switch] Connecting to conductor at "
              << g_identity.conductor.ip << ":" << g_identity.conductor.port << "\n";
}

static void maybe_dial_conductor(NetQueue& queue) {
    if (g_state.conductor) return;
    if (clk::now() - g_last_conductor_dial < CONDUCTOR_RETRY) return;
    dial_conductor(queue);
}

// ---------------------------------------------------------------------------
//  Stats
// ---------------------------------------------------------------------------

static void print_stats(bool with_agents) {
    std::cout << "[Switch] Stats: " << g_blackbox->summary()
              << " link-full-drops=" << g_send_full_drops
              << " tun-write-errors=" << g_tun_write_errors
              << " peers=" << g_state.peer_switches.size()
              << " pending-tokens=" << g_state.pending_tokens.size()
              << " conductor=" << (g_state.conductor && g_state.conductor->connected ? "up" : "down")
              << "\n";
    if (with_agents) {
        std::string lines = g_blackbox->agent_lines();
        if (!lines.empty()) std::cout << "[Switch] Agents:\n" << lines;
    }
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    if (!vnet::tls::init()) return 1;
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, on_sigusr1);
    signal(SIGHUP,  on_sighup);

    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: ./switch <name> [auth_key] <port>\n"
                  << "The key is read from VNET_AUTH_KEY or VNET_AUTH_KEY_FILE "
                  << "when it is not given.\n";
        return 1;
    }

    long hb_sec = g_heartbeat.count(), dead_sec = g_dead.count();
    long cdt_port = 0;
    long stats_sec = 60, agent_pps = 20000, agent_mbps = 200;
    if (!env_int("VNET_HEARTBEAT_SEC", 1, 3600, hb_sec) ||
        !env_int("VNET_DEAD_SEC", 1, 86400, dead_sec) ||
        !env_int("CONDUCTOR_PORT", 1, 65535, cdt_port) ||
        !env_int("VNET_STATS_SEC", 0, 86400, stats_sec) ||
        !env_int("VNET_AGENT_PPS", 0, 100000000, agent_pps) ||
        !env_int("VNET_AGENT_MBPS", 0, 100000, agent_mbps))
        return 1;
    g_heartbeat = std::chrono::seconds(hb_sec);
    g_dead      = std::chrono::seconds(dead_sec);
    g_debug     = env_or("VNET_DEBUG", "0") == "1";
    if (g_dead <= g_heartbeat) {
        std::cerr << "[Switch] VNET_DEAD_SEC must be larger than VNET_HEARTBEAT_SEC\n";
        return 1;
    }
    if (!vnet::tls::enabled() && env_or("VNET_ALLOW_INSECURE", "0") != "1") {
        std::cerr << "[Switch] TLS is off: keys and routes would cross the network "
                  << "in the clear. Set VNET_TLS=1, or VNET_ALLOW_INSECURE=1 for a lab.\n";
        return 1;
    }

    g_switch_name = argv[1];
    if (!valid_name(g_switch_name, SWITCH_NAME_MAX)) {
        std::cerr << "[Switch] Name must be 1 to " << SWITCH_NAME_MAX
                  << " characters from [A-Za-z0-9_-]\n";
        return 1;
    }
    std::string switch_name = g_switch_name;
    std::string auth_key    = load_auth_key(argc == 4 ? argv[2] : nullptr, "[Switch]");
    if (auth_key.empty()) {
        std::cerr << "[Switch] No auth key: set VNET_AUTH_KEY or VNET_AUTH_KEY_FILE\n";
        return 1;
    }
    const char* port_text = argv[argc - 1];
    char* port_end = nullptr;
    long port_long = std::strtol(port_text, &port_end, 10);
    if (port_end == port_text || *port_end != '\0' || port_long < 1 || port_long > 65535) {
        std::cerr << "[Switch] Port must be between 1 and 65535, got '"
                  << port_text << "'\n";
        return 1;
    }
    uint16_t port = static_cast<uint16_t>(port_long);

    // --- Read conductor address from environment ---
    const char* cdt_ip_env  = std::getenv("CONDUCTOR_IP");
    const char* config_path = std::getenv("VNET_CONFIG_PATH");
    if (!cdt_ip_env || !*cdt_ip_env || cdt_port == 0) {
        std::cerr << "[Switch] CONDUCTOR_IP and CONDUCTOR_PORT must be set\n";
        return 1;
    }
    g_overlay_cidr = env_or("VNET_OVERLAY_CIDR", g_overlay_cidr);
    {
        uint32_t addr = 0;
        int prefix = 0;
        if (!parse_cidr(g_overlay_cidr, addr, prefix)) {
            std::cerr << "[Switch] VNET_OVERLAY_CIDR is not a.b.c.d/n: "
                      << g_overlay_cidr << "\n";
            return 1;
        }
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

    const std::string tun_name = "vnet-out";
    int tun_fd = tun_open(tun_name, string_to_ipv4("192.0.2.1"), 32);
    if (tun_fd < 0) {
        std::cerr << "[Switch] Failed to open internet TUN\n";
        return 1;
    }
    if (!setup_internet(tun_name))
        std::cerr << "[Switch] Internet forwarding is not active\n";

    BlackBox blackbox(config, tun_fd);
    blackbox.set_agent_limits(static_cast<uint64_t>(agent_pps),
                              static_cast<uint64_t>(agent_mbps) * 1000000ULL / 8ULL);
    g_blackbox = &blackbox;
    std::cout << "[Switch] Per-agent limit: " << agent_pps << " pps, "
              << agent_mbps << " Mbit/s (0 = none)\n";

    std::cout << "[Switch] Loaded config from " << config_path << "\n";

    g_identity.name        = switch_name;
    g_identity.auth_key    = auth_key;
    g_identity.port        = port;
    g_identity.conductor   = MachineConfig{cdt_ip_env, static_cast<uint16_t>(cdt_port)};
    g_identity.config_path = config_path;
    g_identity.config_hash = config.fingerprint();
    g_identity.network     = env_or("VNET_NETWORK", "::internet");
    {
        // Behind NAT or on a host with several addresses, the address the
        // conductor sees is not the one agents and peers can dial.
        std::string adv = env_or("VNET_ADVERTISE_IP", "");
        if (!adv.empty()) {
            g_identity.advertise_ipv4 = resolve_ipv4(adv);
            if (g_identity.advertise_ipv4 == 0) {
                std::cerr << "[Switch] VNET_ADVERTISE_IP does not resolve: " << adv << "\n";
                return 1;
            }
        }
    }
    std::cout << "[Switch] Config fingerprint " << g_identity.config_hash.substr(0, 12) << "\n";

    int flag = 1;

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

    if (queue.put_tun(tun_fd, nullptr) == nullptr) {
        std::cerr << "[Switch] Failed to add internet TUN to queue\n";
        return 1;
    }

    dial_conductor(queue);
    auto last_stats = clk::now();

    // --- 4. Event loop ---
    while (g_running) {
        maybe_dial_conductor(queue);
        for (int accepted = 0; accepted < ACCEPTS_PER_TURN; accepted++) {
            int agent_fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK);
            if (agent_fd < 0) break;

            setsockopt(agent_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            auto* info = new ConnInfo();
            info->role = ConnRole::AGENT_PENDING;
            info->fd   = agent_fd;

            // The TLS handshake runs inside the queue; the identify
            // timeout closes a socket that never finishes it.
            NetworkElement* element = nullptr;
            if (vnet::tls::enabled()) {
                ssl_st* ssl = vnet::tls::new_session(agent_fd);
                element = queue.put_sck_tls_server(agent_fd, info, ssl);
                if (!element) vnet::tls::free_session(ssl);
            } else {
                element = queue.put_sck(agent_fd, info);
            }
            if (element == nullptr) {
                close(agent_fd);
                delete info;
                continue;
            }
            g_unidentified.insert(info);
        }

        // Process events
        queue.wait_and_process();

        // Expire old tokens and packets that never learned a route
        g_state.expire_tokens();
        expire_pending();

        retry_peers(queue);

        // Heartbeats
        send_heartbeats(queue);
        reap_dead(queue);

        if (g_reload_request) {
            g_reload_request = 0;
            vnet::blackbox::Config fresh;
            if (fresh.load(g_identity.config_path)) {
                blackbox.replace_config(fresh);
                g_identity.config_hash = fresh.fingerprint();
                std::cout << "[Switch] Config reloaded, fingerprint "
                          << g_identity.config_hash.substr(0, 12) << "\n";
                if (g_state.conductor && g_state.conductor->connected) {
                    mip::PacketSwitchConfig cfg_pkt;
                    cfg_pkt.set_config_hash(g_identity.config_hash);
                    queue.send(g_state.conductor->fd, PacketType::SWITCH_CONFIG, cfg_pkt);
                }
            } else {
                std::cerr << "[Switch] Config reload failed; the previous config stays in force\n";
            }
        }

        if (g_stats_request) {
            g_stats_request = 0;
            print_stats(/*with_agents=*/true);
            last_stats = clk::now();
        } else if (stats_sec > 0 && clk::now() - last_stats >= std::chrono::seconds(stats_sec)) {
            print_stats(/*with_agents=*/false);
            last_stats = clk::now();
        }
    }

    print_stats(/*with_agents=*/true);
    close(listener);
    tun_close(tun_fd, tun_name);
    std::cout << "[Switch] Shutting down.\n";
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}

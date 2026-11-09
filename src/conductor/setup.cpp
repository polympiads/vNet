#include <cstdint>
#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "mip.pb.h"
#include "state.pb.h"
#include "common/socket_utils.h"
#include "common/sysutil.h"
#include "vnet/netqueue/handler.hpp"
#include "vnet/protocol/dispatch.hpp"
#include "vnet/netqueue/netqueue.hpp"
#include "vnet/tls/session.hpp"

using namespace vnet::protocol;
using namespace vnet::netqueue;
using clk = std::chrono::steady_clock;
using sysclk = std::chrono::system_clock;

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

static const uint16_t             LISTEN_PORT         = 5000;
static const int                  LISTEN_BACKLOG      = 1024;
/* Handshakes run inside the queue, so a turn can take a whole burst. */
static const int                  ACCEPTS_PER_TURN    = 64;
static const std::chrono::seconds HEARTBEAT_INTERVAL  {30};
static const std::chrono::seconds DUMP_INTERVAL       {5};
static std::chrono::seconds       g_heartbeat = HEARTBEAT_INTERVAL;
static std::chrono::seconds       g_dead{90};
static const std::chrono::seconds IDENTIFY_TIMEOUT    {10};
/* A switch forgets a token after 60 s; forget it here at the same time. */
static const int64_t              TOKEN_EXPIRY_MS      = 60 * 1000;

static const char* DEFAULT_DUMP_PATH = "/var/vnet-dump.bin";

static volatile sig_atomic_t g_running      = 1;
static volatile sig_atomic_t g_dump_request = 0;

static void on_signal(int) {
    g_running = 0;
}
static void on_sigusr1(int) {
    g_dump_request = 1;
}

// ---------------------------------------------------------------------------
//  Per-connection metadata
// ---------------------------------------------------------------------------

enum class Role { UNKNOWN, SWITCH_CONN, AGENT_CONN };

struct ConnInfo {
    Role        role = Role::UNKNOWN;
    std::string name;
    std::string auth_key;
    std::string network;

    /* Switch-only fields */
    uint16_t    sw_port = 0;
    uint32_t    sw_ipv4 = 0;

    /* Agent-only fields */
    std::string assigned_switch_name;
    ConnInfo* assigned_switch = nullptr;

    int             fd = -1;
    uint32_t        virtual_ipv4 = 0;
    clk::time_point last_hb_sent  = clk::now();
    clk::time_point last_rx       = clk::now();
    clk::time_point accepted_at   = clk::now();
    int64_t         connected_at_ms = 0;  // unix timestamp ms
};

/* Accepted sockets that may still be waiting for their MIP. */
static std::unordered_set<ConnInfo*> g_unidentified;

// ---------------------------------------------------------------------------
//  Conductor state
// ---------------------------------------------------------------------------

struct ConductorState {
    std::vector<ConnInfo*> switches;
    std::vector<ConnInfo*> agents;

    // token → (agent_name, switch_name, created_at_ms)
    struct PendingTokenInfo {
        std::string agent_name;
        std::string switch_name;
        int64_t     created_at_ms;
        uint64_t    token;
    };
    std::vector<PendingTokenInfo> pending_tokens;

    struct PublishedRoute {
        std::string agent_name;
        std::string switch_name;
        uint32_t    switch_ipv4 = 0;
        uint32_t    switch_port = 0;
        uint32_t    agent_ipv4  = 0;
    };
    std::vector<PublishedRoute> published;

    size_t rr_index = 0;

    /*
     * Tokens come from the OpenSSL CSPRNG. A seeded PRNG would let an
     * agent that saw its own token predict the tokens handed to every
     * other machine and log in as them.
     */
    uint64_t generate_token() {
        uint64_t token = 0;
        do {
            if (RAND_bytes(reinterpret_cast<unsigned char*>(&token),
                           sizeof(token)) != 1) {
                std::cerr << "[Conductor] RAND_bytes failed\n";
                std::abort();
            }
        } while (token == 0);
        return token;
    }

    void expire_tokens(int64_t now) {
        pending_tokens.erase(
            std::remove_if(pending_tokens.begin(), pending_tokens.end(),
                [&](const PendingTokenInfo& t) {
                    return now - t.created_at_ms > TOKEN_EXPIRY_MS;
                }),
            pending_tokens.end());
    }

    ConnInfo* pick_switch(const std::string& agent_network) {
        if (switches.empty()) return nullptr;

        /* 
         * Spec: the conductor should ensure the agent can theoretically
         * reach the switch (same network OR switch is "::internet").
         * We also round-robin for load balancing.
         */
        size_t start = rr_index;
        for (size_t i = 0; i < switches.size(); i++) {
            size_t idx = (start + i) % switches.size();
            ConnInfo* sw = switches[idx];

            bool reachable =
                sw->network == agent_network ||
                sw->network == "::internet";

            if (reachable) {
                rr_index = idx + 1;
                return sw;
            }
        }
        return nullptr;
    }

    void remove_switch(ConnInfo* info) {
        switches.erase(
            std::remove(switches.begin(), switches.end(), info),
            switches.end());
        // remove any pending tokens for this switch
        pending_tokens.erase(
            std::remove_if(pending_tokens.begin(), pending_tokens.end(),
                [&](const PendingTokenInfo& t) {
                    return t.switch_name == info->name;
                }),
            pending_tokens.end());
    }
    void remove_agent(ConnInfo* info) {
        agents.erase(
            std::remove(agents.begin(), agents.end(), info),
            agents.end());
    }
    void remove_pending_token(uint64_t token) {
        pending_tokens.erase(
            std::remove_if(pending_tokens.begin(), pending_tokens.end(),
                [&](const PendingTokenInfo& t) {
                    return t.token == token;
                }),
            pending_tokens.end());
    }
};

static ConductorState g_state;

// ---------------------------------------------------------------------------
//  State dump
// ---------------------------------------------------------------------------

static std::string g_dump_path;

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        sysclk::now().time_since_epoch()).count();
}

static void write_dump() {
    vnet::state::ConductorState proto;
    proto.set_dumped_at_ms(now_ms());

    for (const auto* sw : g_state.switches) {
        auto* s = proto.add_switches();
        s->set_name(sw->name);
        s->set_network(sw->network);
        s->set_ipv4(ipv4_to_string(sw->sw_ipv4));
        s->set_port(sw->sw_port);
        s->set_connected_at_ms(sw->connected_at_ms);
    }

    for (const auto* ag : g_state.agents) {
        auto* a = proto.add_agents();
        a->set_name(ag->name);
        a->set_assigned_switch(ag->assigned_switch_name);
        a->set_virtual_ipv4(ag->virtual_ipv4);
        a->set_connected_at_ms(ag->connected_at_ms);
    }

    static bool announced = false;
    if (!announced && !g_state.agents.empty()) {
        announced = true;
        for (const auto* ag : g_state.agents) {
            std::cout << "[Conductor] State " << ag->name
                      << " -> " << ag->assigned_switch_name
                      << " " << ipv4_to_string(ag->virtual_ipv4) << "\n";
        }
    }

    for (const auto& pt : g_state.pending_tokens) {
        auto* t = proto.add_pending_tokens();
        t->set_agent_name(pt.agent_name);
        t->set_token(pt.token);
        t->set_switch_name(pt.switch_name);
        t->set_created_at_ms(pt.created_at_ms);
    }

    // Write atomically: temp file + rename
    std::string tmp_path = g_dump_path + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[Conductor] Failed to open dump file: " << tmp_path << "\n";
        return;
    }

    if (!proto.SerializeToOstream(&out)) {
        std::cerr << "[Conductor] Failed to serialize state dump\n";
        return;
    }
    out.close();

    if (rename(tmp_path.c_str(), g_dump_path.c_str()) != 0) {
        std::cerr << "[Conductor] Failed to rename dump file\n";
    }
}

// ---------------------------------------------------------------------------
//  Dispatch
// ---------------------------------------------------------------------------

struct AuthRecord {
    std::string role;
    std::string key;
};
static std::unordered_map<std::string, AuthRecord> g_auth;
static bool g_auth_required = false;

/*
 * What the conductor itself handed out: the key the agent used and the
 * network it named. Lost on restart.
 */
struct RememberedAgent {
    std::string auth_key;
    std::string network;
};
static std::unordered_map<std::string, RememberedAgent> g_remembered;

/*
 * Where each agent is, as the switches report it with AGENT_REGISTERED.
 * Switches re-announce their agents when they register, so this survives
 * a conductor restart once they have reconnected.
 */
struct AgentLocation {
    std::string switch_name;
    uint32_t    virtual_ipv4 = 0;
};
static std::unordered_map<std::string, AgentLocation> g_agent_location;

static bool load_auth_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        if (line[start] == '#') continue;
        std::istringstream iss(line.substr(start));
        std::string role, name, key;
        if (!(iss >> role >> name >> key)) return false;
        if (role != "switch" && role != "agent") return false;
        if (!valid_name(name, role == "agent" ? AGENT_NAME_MAX : SWITCH_NAME_MAX)) {
            std::cerr << "[Conductor] Invalid " << role << " name '" << name
                      << "' in auth file\n";
            return false;
        }
        g_auth[name] = {role, key};
    }
    return !g_auth.empty();
}

/* Equal length and equal bytes, in time that does not depend on where
 * the first difference is. */
static bool keys_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

static bool auth_ok(const std::string& role, const std::string& name,
                    const std::string& key) {
    if (!g_auth_required) return true;
    auto it = g_auth.find(name);
    if (it == g_auth.end()) return false;
    return it->second.role == role && keys_equal(it->second.key, key);
}

struct ConductorDispatch : public Dispatch {
    NetQueue* queue = nullptr;
    void set_queue(NetQueue* q) { queue = q; }

    void onActivity(socket_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (info) info->last_rx = clk::now();
    }

    /* A connection identifies itself once. A second MIP or MRP on it
     * would let a registered machine take on another name or role. */
    bool already_identified(socket_data data, const char* what) {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info || info->role == Role::UNKNOWN) return false;
        std::cerr << "[Conductor] Rejected " << what << " on the already "
                  << "registered connection of " << info->name << "\n";
        queue->close(data.fd);
        return true;
    }

    void onSwitchMIP(socket_data data, mip::PacketSwitchMIP& pkt) override {
        auto* info     = static_cast<ConnInfo*>(data.ptr_data);
        if (already_identified(data, "switch MIP")) return;
        if (!auth_ok("switch", pkt.name(), pkt.auth_key())) {
            std::cerr << "[Conductor] Rejected switch auth: " << pkt.name() << "\n";
            queue->close(data.fd);
            return;
        }
        // A switch whose link to us broke dials again before we notice
        // the old socket is dead. The key holder proved itself, so the
        // new connection wins; closing the old one withdraws its routes,
        // and the re-announcement that follows this MIP restores them.
        int old_fd = -1;
        for (const auto* existing : g_state.switches) {
            if (existing->name == pkt.name() && existing != info) {
                old_fd = existing->fd;
                break;
            }
        }
        if (old_fd >= 0) {
            std::cout << "[Conductor] Switch " << pkt.name()
                      << " registered again; closing fd " << old_fd << "\n";
            queue->close(old_fd);
        }
        info->role     = Role::SWITCH_CONN;
        info->name     = pkt.name();
        info->auth_key = pkt.auth_key();
        info->network  = pkt.network();
        info->sw_port  = static_cast<uint16_t>(pkt.port());
        info->fd       = data.fd;
        info->connected_at_ms = now_ms();

        // Obtain the switch's IP from the TCP peer address
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        if (getpeername(data.fd, (sockaddr*)&peer, &plen) == 0) {
            info->sw_ipv4 = peer.sin_addr.s_addr;
        }

        g_state.switches.push_back(info);

        // One fresh token per pair: whichever side dials shows it in its
        // hello, the other side checks it. Issued before the routes, so a
        // switch usually holds the token before the peer learns its
        // address; a hello that arrives first is refused and redialed.
        for (auto* other : g_state.switches) {
            if (other == info) continue;
            uint64_t token = g_state.generate_token();
            mip::PacketPeerToken to_other;
            to_other.set_peer_name(info->name);
            to_other.set_token(token);
            queue->send(other->fd, PacketType::PEER_TOKEN, to_other);
            mip::PacketPeerToken to_new;
            to_new.set_peer_name(other->name);
            to_new.set_token(token);
            queue->send(info->fd, PacketType::PEER_TOKEN, to_new);
        }

        for (const auto& route : g_state.published) {
            if (route.switch_name == info->name) continue;
            bool owner_up = false;
            for (const auto* sw : g_state.switches) {
                if (sw->name == route.switch_name) { owner_up = true; break; }
            }
            if (!owner_up) continue;

            mip::PacketSwitchRouteUpdate update;
            update.set_switch_name(route.switch_name);
            update.set_switch_ipv4(route.switch_ipv4);
            update.set_switch_port(route.switch_port);
            update.set_agent_ipv4(route.agent_ipv4);
            queue->send(info->fd, PacketType::SWITCH_ROUTE_UPDATE, update);
        }

        std::cout << "[Conductor] Switch registered: " << info->name
                  << " @ " << ipv4_to_string(info->sw_ipv4)
                  << ":" << info->sw_port
                  << " (network=" << info->network << ")\n";
    }

    void onAgentMIP(socket_data data, mip::PacketAgentMIP& pkt) override {
        auto* info     = static_cast<ConnInfo*>(data.ptr_data);
        if (already_identified(data, "agent MIP")) return;
        if (!auth_ok("agent", pkt.name(), pkt.auth_key())) {
            std::cerr << "[Conductor] Rejected agent auth: " << pkt.name() << "\n";
            queue->close(data.fd);
            return;
        }
        info->role     = Role::AGENT_CONN;
        info->name     = pkt.name();
        info->auth_key = pkt.auth_key();
        info->network  = pkt.network();
        info->fd       = data.fd;
        info->connected_at_ms = now_ms();

        // The key holder proved itself again: the newer connection wins,
        // as with MRP. An agent whose switch link dropped closes its
        // conductor socket and re-registers, and the close may still be
        // in flight when the new MIP arrives.
        int old_fd = -1;
        for (const auto* existing : g_state.agents) {
            if (existing->name == pkt.name() && existing != info) {
                old_fd = existing->fd;
                break;
            }
        }
        if (old_fd >= 0) {
            std::cout << "[Conductor] Agent " << pkt.name()
                      << " registered again; closing fd " << old_fd << "\n";
            queue->close(old_fd);
        }

        ConnInfo* sw = g_state.pick_switch(pkt.network());
        if (!sw) {
            std::cerr << "[Conductor] No available switch for agent "
                      << pkt.name() << "\n";
            queue->close(data.fd);
            return;
        }

        info->assigned_switch_name  = sw->name;
        info->assigned_switch       = sw;

        uint64_t token = g_state.generate_token();

        std::cout << "[Conductor] Agent " << pkt.name()
                  << " -> switch " << sw->name << "\n";

        // 1) Tell the agent which switch to connect to
        mip::PacketConnectToSwitch resp;
        resp.set_switch_name(sw->name);
        resp.set_switch_ipv4(sw->sw_ipv4);
        resp.set_connection_token(token);
        resp.set_switch_port(sw->sw_port);

        if (!queue->send(data.fd, PacketType::CONNECT_TO_SWITCH, resp)) {
            std::cerr << "[Conductor] Failed to send switch assignment to agent "
                      << pkt.name() << "\n";
            return;
        }

        // 2) Tell the switch to expect this agent (three-party handshake)
        mip::PacketAgentConnectionToken notify;
        notify.set_agent_name(pkt.name());
        notify.set_connection_token(token);

        if (!queue->send(sw->fd, PacketType::AGENT_CONNECTION_TOKEN, notify)) {
            std::cerr << "[Conductor] Failed to notify switch " << sw->name
                      << " of agent " << pkt.name() << "\n";
            return;
        }

        // Track pending token for dump
        g_state.pending_tokens.push_back({
            pkt.name(), sw->name, now_ms(), token
        });

        g_state.agents.push_back(info);
        g_remembered[info->name] = { info->auth_key, info->network };
    }

    void onAgentMRP(socket_data data, mip::PacketAgentMRP& pkt) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (already_identified(data, "agent MRP")) return;
        if (!auth_ok("agent", pkt.name(), pkt.auth_key())) {
            std::cerr << "[Conductor] Rejected agent reauth: " << pkt.name() << "\n";
            queue->close(data.fd);
            return;
        }

        int old_fd = -1;
        for (const auto* existing : g_state.agents) {
            if (existing->name == pkt.name() && existing != info) {
                old_fd = existing->fd;
                break;
            }
        }
        if (old_fd >= 0) queue->close(old_fd);

        info->role     = Role::AGENT_CONN;
        info->name     = pkt.name();
        info->auth_key = pkt.auth_key();
        info->fd       = data.fd;
        info->connected_at_ms = now_ms();

        // Without an auth file, the key an agent first used is the only
        // thing that ties a rejoin to it; hold it to that when known.
        auto mem = g_remembered.find(pkt.name());
        if (!g_auth_required && mem != g_remembered.end() &&
            !keys_equal(mem->second.auth_key, pkt.auth_key())) {
            std::cerr << "[Conductor] Rejected agent reauth: " << pkt.name()
                      << " (key changed)\n";
            queue->close(data.fd);
            return;
        }
        if (mem != g_remembered.end()) info->network = mem->second.network;

        // The switch that holds the agent told us so, before or after a
        // restart here; its link must be up for the rejoin to mean anything.
        ConnInfo* sw = nullptr;
        auto loc = g_agent_location.find(pkt.name());
        if (loc != g_agent_location.end()) {
            info->assigned_switch_name = loc->second.switch_name;
            info->virtual_ipv4         = loc->second.virtual_ipv4;
            for (auto* candidate : g_state.switches) {
                if (candidate->name == loc->second.switch_name) {
                    sw = candidate;
                    break;
                }
            }
        }
        info->assigned_switch = sw;

        if (!sw) {
            std::cerr << "[Conductor] Agent " << pkt.name()
                      << " reconnected without a live switch\n";
            queue->close(data.fd);
            return;
        }

        g_state.agents.push_back(info);
        std::cout << "[Conductor] Agent reconnected: " << pkt.name()
                  << " on " << sw->name << "\n";
    }

    void onHeartbeat(socket_data) override {}

    void onClose(close_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info) return;

        if (info->role != Role::UNKNOWN) {
            const char* reason_str =
                data.reason == CLOSE_HANGUP ? "hangup" :
                data.reason == CLOSE_ERROR  ? "error"  : "epoll";

            std::cout << "[Conductor] Connection closed: " << info->name
                      << " (fd=" << data.fd
                      << ", reason=" << reason_str << ")\n";
        }
        
        if (info->role == Role::SWITCH_CONN) {
            // Null out assigned_switch for all agents pointing to this switch
            for (auto* agent : g_state.agents) {
                if (agent->assigned_switch == info) {
                    agent->assigned_switch = nullptr;
                }
            }
            // Broadcast disconnection to all remaining switches
            mip::PacketSwitchDisconnected disc;
            disc.set_switch_name(info->name);
            for (auto* sw : g_state.switches) {
                if (sw == info) continue;
                queue->send(sw->fd, PacketType::SWITCH_DISCONNECTED, disc);
            }
            
            g_state.published.erase(
                std::remove_if(g_state.published.begin(), g_state.published.end(),
                    [&](const ConductorState::PublishedRoute& route) {
                        return route.switch_name == info->name;
                    }),
                g_state.published.end());
            g_state.remove_switch(info);
        } else if (info->role == Role::AGENT_CONN) {
            g_state.remove_agent(info);
        }

        g_unidentified.erase(info);
        delete info;
    }

    void onAgentRegistered(socket_data data,
                       mip::PacketAgentRegistered& pkt) override {
        auto* sw_info = static_cast<ConnInfo*>(data.ptr_data);
        if (!sw_info || sw_info->role != Role::SWITCH_CONN) return;

        uint32_t virtual_ipv4 = pkt.virtual_ipv4();

        for (auto* agent : g_state.agents) {
            if (agent->name == pkt.agent_name()) {
                agent->virtual_ipv4 = virtual_ipv4;
                agent->assigned_switch_name = sw_info->name;
                agent->assigned_switch = sw_info;
            }
        }
        g_agent_location[pkt.agent_name()] = { sw_info->name, virtual_ipv4 };

        std::cout << "[Conductor] Agent " << pkt.agent_name()
                << " registered with IP " << ipv4_to_string(virtual_ipv4)
                << " on switch " << sw_info->name << "\n";

        g_state.pending_tokens.erase(
            std::remove_if(g_state.pending_tokens.begin(), g_state.pending_tokens.end(),
                [&](const ConductorState::PendingTokenInfo& t) {
                    return t.agent_name == pkt.agent_name();
                }),
            g_state.pending_tokens.end());

        g_state.published.erase(
            std::remove_if(g_state.published.begin(), g_state.published.end(),
                [&](const ConductorState::PublishedRoute& route) {
                    return route.agent_name == pkt.agent_name() ||
                           route.agent_ipv4 == virtual_ipv4;
                }),
            g_state.published.end());
        g_state.published.push_back({
            pkt.agent_name(), sw_info->name, sw_info->sw_ipv4,
            sw_info->sw_port, virtual_ipv4
        });

        // Broadcast route to all other switches
        mip::PacketSwitchRouteUpdate update;
        update.set_switch_name(sw_info->name);
        update.set_switch_ipv4(sw_info->sw_ipv4);
        update.set_switch_port(sw_info->sw_port);
        update.set_agent_ipv4(virtual_ipv4);

        for (auto* sw : g_state.switches) {
            if (sw->fd == data.fd) continue;  // don't send back to the same switch
            queue->send(sw->fd, PacketType::SWITCH_ROUTE_UPDATE, update);
        }
    }

    void onAgentUnregistered(socket_data data,
                             mip::PacketAgentUnregistered& pkt) override {
        auto* sw_info = static_cast<ConnInfo*>(data.ptr_data);
        if (!sw_info || sw_info->role != Role::SWITCH_CONN) return;

        g_state.published.erase(
            std::remove_if(g_state.published.begin(), g_state.published.end(),
                [&](const ConductorState::PublishedRoute& route) {
                    return route.agent_ipv4 == pkt.virtual_ipv4() ||
                           route.agent_name == pkt.agent_name();
                }),
            g_state.published.end());

        auto loc = g_agent_location.find(pkt.agent_name());
        if (loc != g_agent_location.end() && loc->second.switch_name == sw_info->name)
            g_agent_location.erase(loc);

        std::cout << "[Conductor] Agent " << pkt.agent_name()
                  << " left " << sw_info->name << "\n";

        for (auto* sw : g_state.switches) {
            if (sw->fd == data.fd) continue;
            queue->send(sw->fd, PacketType::AGENT_UNREGISTERED, pkt);
        }
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

    for (auto* c : g_state.switches) try_hb(c);
    for (auto* c : g_state.agents)   try_hb(c);
}

static void reap_dead(NetQueue& queue) {
    auto now = clk::now();
    std::vector<int> dead;
    auto check = [&](ConnInfo* c) {
        if (c && c->fd >= 0 && now - c->last_rx >= g_dead)
            dead.push_back(c->fd);
    };
    for (auto* c : g_state.switches) check(c);
    for (auto* c : g_state.agents) check(c);
    for (int fd : dead) {
        std::cerr << "[Conductor] Heartbeat timeout on fd " << fd << "\n";
        queue.close(fd);
    }

    // A socket that never sends its MIP holds an fd and is never
    // heartbeat-checked, since it is in neither list above.
    std::vector<int> silent;
    for (auto it = g_unidentified.begin(); it != g_unidentified.end(); ) {
        ConnInfo* c = *it;
        if (c->role != Role::UNKNOWN) {
            it = g_unidentified.erase(it);
            continue;
        }
        if (now - c->accepted_at >= IDENTIFY_TIMEOUT)
            silent.push_back(c->fd);
        ++it;
    }
    for (int fd : silent) {
        std::cerr << "[Conductor] No MIP within "
                  << IDENTIFY_TIMEOUT.count() << "s on fd " << fd << "\n";
        queue.close(fd);
    }
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    if (!vnet::tls::init()) return 1;
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    signal(SIGINT,   on_signal);
    signal(SIGTERM,  on_signal);
    signal(SIGPIPE,  SIG_IGN);
    signal(SIGUSR1,  on_sigusr1);

    // Dump path from env or default
    const char* dump_env = std::getenv("VNET_DUMP_PATH");
    g_dump_path = dump_env ? dump_env : DEFAULT_DUMP_PATH;
    std::cout << "[Conductor] State dump path: " << g_dump_path << "\n";

    long hb_sec = g_heartbeat.count(), dead_sec = g_dead.count();
    if (!env_int("VNET_HEARTBEAT_SEC", 1, 3600, hb_sec) ||
        !env_int("VNET_DEAD_SEC", 1, 86400, dead_sec))
        return 1;
    g_heartbeat = std::chrono::seconds(hb_sec);
    g_dead      = std::chrono::seconds(dead_sec);

    if (const char* auth_path = std::getenv("VNET_AUTH_FILE")) {
        g_auth_required = true;
        if (!load_auth_file(auth_path)) {
            std::cerr << "[Conductor] Failed to load auth file " << auth_path << "\n";
            return 1;
        }
        std::cout << "[Conductor] Loaded " << g_auth.size() << " auth record(s)\n";
    }

    {
        std::ifstream previous(g_dump_path, std::ios::binary);
        if (previous) {
            vnet::state::ConductorState prior;
            if (prior.ParseFromIstream(&previous)) {
                std::cout << "[Conductor] Previous dump had "
                          << prior.switches_size() << " switch(es) and "
                          << prior.agents_size() << " agent(s)\n";
            }
        }
    }

    // --- Create listener ---
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listener < 0) { perror("socket"); return 1; }

    int optval = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listener, LISTEN_BACKLOG) < 0) {
        perror("listen"); return 1;
    }

    std::cout << "[Conductor] Listening on port " << LISTEN_PORT << "\n";

    // --- NetQueue setup ---
    ConductorDispatch dispatch;
    NetworkQueueHandler handler = makeNetworkQueueHandler(&dispatch);
    NetQueue queue(handler, /*epoll_timeout_ms=*/200);
    dispatch.set_queue(&queue);

    auto last_dump = clk::now();

    // --- Event loop ---
    while (g_running) {
        for (int accepted = 0; accepted < ACCEPTS_PER_TURN; accepted++) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int client = accept4(listener, (sockaddr*)&peer, &plen, SOCK_NONBLOCK);
            if (client < 0) break;

            int flag = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            auto* info = new ConnInfo();
            info->fd = client;

            // The TLS handshake runs inside the queue as the socket
            // becomes readable, so a silent peer stalls nothing. The
            // identify timeout closes it if it never finishes.
            NetworkElement* element = nullptr;
            if (vnet::tls::enabled()) {
                ssl_st* ssl = vnet::tls::new_session(client);
                element = queue.put_sck_tls_server(client, info, ssl);
                if (!element) vnet::tls::free_session(ssl);
            } else {
                element = queue.put_sck(client, info);
            }
            if (element == nullptr) {
                std::cerr << "[Conductor] Failed to add fd " << client << " to queue\n";
                close(client);
                delete info;
                continue;
            }
            g_unidentified.insert(info);
        }

        // Process epoll events
        queue.wait_and_process();

        // Heartbeats
        send_heartbeats(queue);
        reap_dead(queue);
        g_state.expire_tokens(now_ms());

        // Periodic dump
        auto now = clk::now();
        if (now - last_dump >= DUMP_INTERVAL) {
            write_dump();
            last_dump = now;
        }

        // On-demand dump via SIGUSR1
        if (g_dump_request) {
            g_dump_request = 0;
            write_dump();
        }
    }

    close(listener);
    write_dump();  // final dump on shutdown
    std::cout << "[Conductor] Shutting down.\n";
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}

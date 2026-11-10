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
#include "vnet/blackbox/config.hpp"
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

static const long                 DEFAULT_PORT        = 5000;
static const int                  LISTEN_BACKLOG      = 1024;
/* Handshakes run inside the queue, so a turn can take a whole burst. */
static const int                  ACCEPTS_PER_TURN    = 64;
/* A dead peer is noticed within g_dead; both can be set per process. */
static std::chrono::seconds       g_heartbeat{10};
static std::chrono::seconds       g_dead{30};
static std::chrono::seconds       g_dump_interval{60};
static const std::chrono::seconds IDENTIFY_TIMEOUT    {10};
/* A switch forgets a token after 60 s; forget it here at the same time. */
static const int64_t              TOKEN_EXPIRY_MS      = 60 * 1000;

static const char* DEFAULT_DUMP_PATH = "/var/vnet-dump.bin";

static volatile sig_atomic_t g_running        = 1;
static volatile sig_atomic_t g_dump_request   = 0;
static volatile sig_atomic_t g_reload_request = 0;

static void on_signal(int) {
    g_running = 0;
}
static void on_sigusr1(int) {
    g_dump_request = 1;
}
static void on_sighup(int) {
    g_reload_request = 1;
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
    std::string config_hash;

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
    /* The same connections by name, for lookups that must not walk
     * every agent when a thousand of them join at once. */
    std::unordered_map<std::string, ConnInfo*> switch_by_name;
    std::unordered_map<std::string, ConnInfo*> agent_by_name;

    ConnInfo* find_switch(const std::string& name) const {
        auto it = switch_by_name.find(name);
        return it == switch_by_name.end() ? nullptr : it->second;
    }
    ConnInfo* find_agent(const std::string& name) const {
        auto it = agent_by_name.find(name);
        return it == agent_by_name.end() ? nullptr : it->second;
    }
    void add_switch(ConnInfo* info) {
        switches.push_back(info);
        switch_by_name[info->name] = info;
    }
    void add_agent(ConnInfo* info) {
        agents.push_back(info);
        agent_by_name[info->name] = info;
    }

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
        auto by_name = switch_by_name.find(info->name);
        if (by_name != switch_by_name.end() && by_name->second == info)
            switch_by_name.erase(by_name);
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
        auto by_name = agent_by_name.find(info->name);
        if (by_name != agent_by_name.end() && by_name->second == info)
            agent_by_name.erase(by_name);
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
/* Every switch must apply the same ACL; VNET_CONFIG_HASH_CHECK=0 turns this off. */
static bool g_config_hash_check = true;

/*
 * The switch config, when VNET_CONFIG_PATH is set here: the conductor
 * hands it to every switch as it registers and again after a reload, so
 * one edit on this machine reaches the whole network. Without it the
 * switches read their own files and are only compared.
 */
struct PushedConfig {
    std::string path;
    std::string text;
    std::string hash;
    bool        enabled() const { return !path.empty(); }
};
static PushedConfig g_config;

/* One frame holds at most MAX_PAYLOAD_SIZE bytes; keep the protobuf
 * overhead and the hash well clear of it. */
static const size_t CONFIG_PIECE_BYTES = 60000;

/*
 * Read and parse the config file into a fresh copy, swapped in only
 * when the whole file parsed, so a reload that fails keeps the old one.
 */
static bool load_config_file() {
    std::ifstream in(g_config.path, std::ios::binary);
    if (!in) {
        std::cerr << "[Conductor] Cannot open config file " << g_config.path << "\n";
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    vnet::blackbox::Config parsed;
    if (!parsed.load_text(buf.str())) {
        std::cerr << "[Conductor] Config file " << g_config.path << " did not parse\n";
        return false;
    }
    if (parsed.agents().empty())
        std::cerr << "[Conductor] Config file " << g_config.path
                  << " names no agent; no machine can join\n";
    g_config.text = buf.str();
    g_config.hash = parsed.fingerprint();
    return true;
}

/* Send the config to one switch, in pieces that each fit a frame. */
static bool send_config(NetQueue& queue, int fd) {
    const std::string& text = g_config.text;
    uint32_t index = 0;
    size_t offset = 0;
    do {
        size_t n = std::min(CONFIG_PIECE_BYTES, text.size() - offset);
        mip::PacketConfigPush piece;
        piece.set_config_hash(g_config.hash);
        piece.set_index(index++);
        piece.set_text(text.data() + offset, n);
        offset += n;
        piece.set_last(offset >= text.size());
        if (!queue.send(fd, PacketType::CONFIG_PUSH, piece)) return false;
    } while (offset < text.size());
    return true;
}

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

/*
 * Read the auth file into a fresh table and swap it in only when the
 * whole file parsed, so a reload that fails keeps the old table.
 */
static bool load_auth_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "[Conductor] Cannot open auth file " << path << "\n";
        return false;
    }
    std::unordered_map<std::string, AuthRecord> fresh;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        line_no++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        if (line[start] == '#') continue;
        std::istringstream iss(line.substr(start));
        std::string role, name, key;
        if (!(iss >> role >> name >> key) || (role != "switch" && role != "agent")) {
            std::cerr << "[Conductor] Auth file line " << line_no
                      << ": expected 'switch <name> <key>' or 'agent <name> <key>'\n";
            return false;
        }
        if (!valid_name(name, role == "agent" ? AGENT_NAME_MAX : SWITCH_NAME_MAX)) {
            std::cerr << "[Conductor] Auth file line " << line_no << ": invalid "
                      << role << " name '" << name << "'\n";
            return false;
        }
        if (fresh.count(name)) {
            std::cerr << "[Conductor] Auth file line " << line_no << ": " << name
                      << " is listed twice\n";
            return false;
        }
        fresh[name] = {role, key};
    }
    if (fresh.empty()) {
        std::cerr << "[Conductor] Auth file " << path << " names no machine\n";
        return false;
    }
    g_auth = std::move(fresh);
    return true;
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

    /* With TLS on, the certificate must name the role the peer claims. */
    bool cert_allows(socket_data data, const char* role, const std::string& name) {
        if (vnet::tls::peer_has_role(data.net_element->ssl, role)) return true;
        std::cerr << "[Conductor] Rejected " << role << " " << name
                  << ": certificate role is '"
                  << vnet::tls::peer_role(data.net_element->ssl) << "'\n";
        queue->close(data.fd);
        return false;
    }

    void onSwitchMIP(socket_data data, mip::PacketSwitchMIP& pkt) override {
        auto* info     = static_cast<ConnInfo*>(data.ptr_data);
        if (already_identified(data, "switch MIP")) return;
        if (!cert_allows(data, vnet::tls::ROLE_SWITCH, pkt.name())) return;
        if (!auth_ok("switch", pkt.name(), pkt.auth_key())) {
            std::cerr << "[Conductor] Rejected switch auth: " << pkt.name() << "\n";
            queue->close(data.fd);
            return;
        }
        // A switch whose link to us broke dials again before we notice
        // the old socket is dead. The key holder proved itself, so the
        // new connection wins; closing the old one withdraws its routes,
        // and the re-announcement that follows this MIP restores them.
        ConnInfo* existing = g_state.find_switch(pkt.name());
        if (existing && existing != info) {
            std::cout << "[Conductor] Switch " << pkt.name()
                      << " registered again; closing fd " << existing->fd << "\n";
            queue->close(existing->fd);
        }

        // A switch with a different config would apply a different ACL
        // to the same machines. When the switches bring their own files,
        // refuse it and say which switch disagrees. When the config comes
        // from here, it is sent right after this and checked when the
        // switch confirms it.
        if (g_config_hash_check && !g_config.enabled() && !pkt.config_hash().empty()) {
            for (const auto* other : g_state.switches) {
                if (other->config_hash.empty() || other->config_hash == pkt.config_hash())
                    continue;
                std::cerr << "[Conductor] Rejected switch " << pkt.name()
                          << ": its config (" << pkt.config_hash().substr(0, 12)
                          << ") differs from " << other->name << "'s ("
                          << other->config_hash.substr(0, 12) << ")\n";
                queue->close(data.fd);
                return;
            }
        }
        info->config_hash = pkt.config_hash();
        info->role     = Role::SWITCH_CONN;
        info->name     = pkt.name();
        info->auth_key = pkt.auth_key();
        info->network  = pkt.network();
        info->sw_port  = static_cast<uint16_t>(pkt.port());
        info->fd       = data.fd;
        info->connected_at_ms = now_ms();

        // The address agents and peers dial: what the switch advertises,
        // or where its connection came from.
        if (pkt.advertise_ipv4() != 0) {
            info->sw_ipv4 = pkt.advertise_ipv4();
        } else {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            if (getpeername(data.fd, (sockaddr*)&peer, &plen) == 0) {
                info->sw_ipv4 = peer.sin_addr.s_addr;
            }
        }

        g_state.add_switch(info);

        // The config goes first on the link, so the switch holds it
        // before the first token or route reaches it.
        if (g_config.enabled()) {
            if (send_config(*queue, info->fd))
                std::cout << "[Conductor] Config " << g_config.hash.substr(0, 12)
                          << " sent to " << info->name << "\n";
            else
                std::cerr << "[Conductor] Failed to send the config to "
                          << info->name << "\n";
        }

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
            if (!g_state.find_switch(route.switch_name)) continue;

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
        if (!cert_allows(data, vnet::tls::ROLE_AGENT, pkt.name())) return;
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
        ConnInfo* existing = g_state.find_agent(pkt.name());
        if (existing && existing != info) {
            std::cout << "[Conductor] Agent " << pkt.name()
                      << " registered again; closing fd " << existing->fd << "\n";
            queue->close(existing->fd);
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

        g_state.add_agent(info);
        g_remembered[info->name] = { info->auth_key, info->network };
    }

    void onAgentMRP(socket_data data, mip::PacketAgentMRP& pkt) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (already_identified(data, "agent MRP")) return;
        if (!cert_allows(data, vnet::tls::ROLE_AGENT, pkt.name())) return;
        if (!auth_ok("agent", pkt.name(), pkt.auth_key())) {
            std::cerr << "[Conductor] Rejected agent reauth: " << pkt.name() << "\n";
            queue->close(data.fd);
            return;
        }

        ConnInfo* existing = g_state.find_agent(pkt.name());
        if (existing && existing != info) queue->close(existing->fd);

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
            sw = g_state.find_switch(loc->second.switch_name);
        }
        info->assigned_switch = sw;

        if (!sw) {
            std::cerr << "[Conductor] Agent " << pkt.name()
                      << " reconnected without a live switch\n";
            queue->close(data.fd);
            return;
        }

        g_state.add_agent(info);
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

        if (ConnInfo* agent = g_state.find_agent(pkt.agent_name())) {
            agent->virtual_ipv4 = virtual_ipv4;
            agent->assigned_switch_name = sw_info->name;
            agent->assigned_switch = sw_info;
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

    /*
     * A switch says which config it applies: after a reload of its own
     * file, or after a config sent from here was applied or refused.
     * With a pushed config the reference is this conductor's; otherwise
     * the switches are compared with each other. Rolling reloads
     * disagree for a while; that is logged, not refused.
     */
    void onSwitchConfig(socket_data data, mip::PacketSwitchConfig& pkt) override {
        auto* sw_info = static_cast<ConnInfo*>(data.ptr_data);
        if (!sw_info || sw_info->role != Role::SWITCH_CONN) return;
        sw_info->config_hash = pkt.config_hash();
        if (g_config.enabled()) {
            if (pkt.config_hash() == g_config.hash) {
                std::cout << "[Conductor] Switch " << sw_info->name
                          << " applied config " << g_config.hash.substr(0, 12) << "\n";
            } else {
                std::cerr << "[Conductor] Switch " << sw_info->name << " runs config "
                          << pkt.config_hash().substr(0, 12) << ", not the one sent ("
                          << g_config.hash.substr(0, 12) << "); see its log\n";
            }
            return;
        }
        std::cout << "[Conductor] Switch " << sw_info->name << " reloaded its config ("
                  << pkt.config_hash().substr(0, 12) << ")\n";
        for (const auto* other : g_state.switches) {
            if (other == sw_info || other->config_hash.empty()) continue;
            if (other->config_hash != sw_info->config_hash)
                std::cerr << "[Conductor] Config mismatch: " << sw_info->name
                          << " and " << other->name << " apply different ACLs\n";
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
    signal(SIGHUP,   on_sighup);

    // Dump path from env or default
    const char* dump_env = std::getenv("VNET_DUMP_PATH");
    g_dump_path = dump_env ? dump_env : DEFAULT_DUMP_PATH;
    std::cout << "[Conductor] State dump path: " << g_dump_path << "\n";

    long hb_sec = g_heartbeat.count(), dead_sec = g_dead.count();
    long dump_sec = g_dump_interval.count(), port = DEFAULT_PORT;
    if (!env_int("VNET_HEARTBEAT_SEC", 1, 3600, hb_sec) ||
        !env_int("VNET_DEAD_SEC", 1, 86400, dead_sec) ||
        !env_int("VNET_DUMP_SEC", 1, 86400, dump_sec) ||
        !env_int("CONDUCTOR_PORT", 1, 65535, port))
        return 1;
    g_heartbeat     = std::chrono::seconds(hb_sec);
    g_dead          = std::chrono::seconds(dead_sec);
    g_dump_interval = std::chrono::seconds(dump_sec);
    if (g_dead <= g_heartbeat) {
        std::cerr << "[Conductor] VNET_DEAD_SEC must be larger than VNET_HEARTBEAT_SEC\n";
        return 1;
    }

    // An event runs with an auth file and TLS. Running without either is
    // for a lab, and must be asked for.
    bool allow_insecure = env_or("VNET_ALLOW_INSECURE", "0") == "1";
    if (!std::getenv("VNET_AUTH_FILE") && !allow_insecure) {
        std::cerr << "[Conductor] No VNET_AUTH_FILE: every name and key would be "
                  << "accepted. Set it, or VNET_ALLOW_INSECURE=1 for a lab.\n";
        return 1;
    }
    if (!vnet::tls::enabled() && !allow_insecure) {
        std::cerr << "[Conductor] TLS is off: keys and routes would cross the network "
                  << "in the clear. Set VNET_TLS=1, or VNET_ALLOW_INSECURE=1 for a lab.\n";
        return 1;
    }

    g_config_hash_check = env_or("VNET_CONFIG_HASH_CHECK", "1") != "0";

    g_config.path = env_or("VNET_CONFIG_PATH", "");
    if (g_config.enabled()) {
        if (!load_config_file()) return 1;
        std::cout << "[Conductor] Switch config " << g_config.path << " ("
                  << g_config.text.size() << " bytes, fingerprint "
                  << g_config.hash.substr(0, 12) << ") will be sent to every switch\n";
    } else {
        std::cout << "[Conductor] No VNET_CONFIG_PATH: switches read their own config files\n";
    }

    std::string auth_path;
    if (const char* env_auth = std::getenv("VNET_AUTH_FILE")) {
        auth_path = env_auth;
        g_auth_required = true;
        if (!load_auth_file(auth_path)) return 1;
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
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listener, LISTEN_BACKLOG) < 0) {
        perror("listen"); return 1;
    }

    std::cout << "[Conductor] Listening on port " << port << "\n";

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
        if (now - last_dump >= g_dump_interval) {
            write_dump();
            last_dump = now;
        }

        // On-demand dump via SIGUSR1
        if (g_dump_request) {
            g_dump_request = 0;
            write_dump();
        }

        // SIGHUP: machines added to the auth file can join without a
        // restart. Machines removed are refused at their next MIP or MRP;
        // the ones connected stay until then. The switch config, when it
        // is held here, is read again and sent to every switch.
        if (g_reload_request) {
            g_reload_request = 0;
            if (!g_auth_required) {
                std::cerr << "[Conductor] No auth file to reload\n";
            } else if (load_auth_file(auth_path)) {
                std::cout << "[Conductor] Auth file reloaded: " << g_auth.size()
                          << " record(s)\n";
            } else {
                std::cerr << "[Conductor] Auth file reload failed; the previous "
                          << "records stay in force\n";
            }
            if (g_config.enabled()) {
                std::string previous = g_config.hash;
                if (!load_config_file()) {
                    std::cerr << "[Conductor] Config reload failed; the previous "
                              << "config stays in force\n";
                } else if (g_config.hash == previous) {
                    std::cout << "[Conductor] Config reloaded, unchanged ("
                              << g_config.hash.substr(0, 12) << ")\n";
                } else {
                    size_t sent = 0;
                    for (auto* sw : g_state.switches)
                        if (send_config(queue, sw->fd)) sent++;
                    std::cout << "[Conductor] Config reloaded, fingerprint "
                              << g_config.hash.substr(0, 12) << "; sent to " << sent
                              << " switch(es)\n";
                }
            }
        }
    }

    close(listener);
    write_dump();  // final dump on shutdown
    std::cout << "[Conductor] Shutting down.\n";
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}

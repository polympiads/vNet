#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <fstream>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>

#include "mip.pb.h"
#include "state.pb.h"
#include "common/socket_utils.h"
#include "vnet/protocol/dispatch.hpp"
#include "vnet/netqueue/netqueue.hpp"

using namespace vnet::protocol;
using namespace vnet::netqueue;
using clk = std::chrono::steady_clock;
using sysclk = std::chrono::system_clock;

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

static const uint16_t             LISTEN_PORT         = 5000;
static const int                  LISTEN_BACKLOG      = 64;
static const std::chrono::seconds HEARTBEAT_INTERVAL  {30};
static const std::chrono::seconds DUMP_INTERVAL       {5};

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

    int         fd = -1;
    clk::time_point last_hb_sent  = clk::now();
    int64_t         connected_at_ms = 0;  // unix timestamp ms
};

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

    size_t rr_index = 0;

    std::mt19937_64 rng{std::random_device{}()};

    uint64_t generate_token() {
        return rng();
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
        // Fallback: round-robin without network check
        ConnInfo* sw = switches[start % switches.size()];
        rr_index = start + 1;
        return sw;
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
        a->set_connected_at_ms(ag->connected_at_ms);
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

struct ConductorDispatch : public Dispatch {
    NetQueue* queue = nullptr;
    void set_queue(NetQueue* q) { queue = q; }

    void onSwitchMIP(socket_data data, mip::PacketSwitchMIP& pkt) override {
        auto* info     = static_cast<ConnInfo*>(data.ptr_data);
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

        std::cout << "[Conductor] Switch registered: " << info->name
                  << " @ " << ipv4_to_string(info->sw_ipv4)
                  << ":" << info->sw_port
                  << " (network=" << info->network << ")\n";
    }

    void onAgentMIP(socket_data data, mip::PacketAgentMIP& pkt) override {
        auto* info     = static_cast<ConnInfo*>(data.ptr_data);
        info->role     = Role::AGENT_CONN;
        info->name     = pkt.name();
        info->auth_key = pkt.auth_key();
        info->network  = pkt.network();
        info->fd       = data.fd;
        info->connected_at_ms = now_ms();

        // Reject if an agent with this name is already connected
        for (const auto* existing : g_state.agents) {
            if (existing->name == pkt.name()) {
                std::cerr << "[Conductor] Rejected duplicate agent: "
                        << pkt.name() << "\n";
                queue->close(data.fd);
                return;
            }
        }

        ConnInfo* sw = g_state.pick_switch(pkt.network());
        if (!sw) {
            std::cerr << "[Conductor] No available switch for agent "
                      << pkt.name() << "\n";
            queue->close(data.fd);
            return;
        }

        uint64_t token = g_state.generate_token();

        std::cout << "[Conductor] Agent " << pkt.name()
                  << " → switch " << sw->name
                  << " (token=" << token << ")\n";

        // 1) Tell the agent which switch to connect to
        mip::PacketConnectToSwitch resp;
        resp.set_switch_name(sw->name);
        resp.set_switch_ipv4(sw->sw_ipv4);
        resp.set_connection_token(token);
        resp.set_switch_port(sw->sw_port);

        if (!send_protobuf_packet(data.fd, PacketType::CONNECT_TO_SWITCH, resp)) {
            std::cerr << "[Conductor] Failed to send switch assignment to agent "
                      << pkt.name() << "\n";
            return;
        }

        // 2) Tell the switch to expect this agent (three-party handshake)
        mip::PacketAgentConnectionToken notify;
        notify.set_agent_name(pkt.name());
        notify.set_connection_token(token);

        if (!send_protobuf_packet(sw->fd, PacketType::AGENT_CONNECTION_TOKEN, notify)) {
            std::cerr << "[Conductor] Failed to notify switch " << sw->name
                      << " of agent " << pkt.name() << "\n";
            return;
        }

        // Track pending token for dump
        g_state.pending_tokens.push_back({
            pkt.name(), sw->name, now_ms(), token
        });

        g_state.agents.push_back(info);
    }

    void onAgentMRP(socket_data data, mip::PacketAgentMRP& pkt) override {
        auto* info     = static_cast<ConnInfo*>(data.ptr_data);
        info->role     = Role::AGENT_CONN;
        info->name     = pkt.name();
        info->auth_key = pkt.auth_key();
        info->fd       = data.fd;
        info->connected_at_ms = now_ms();

        /* 
         * Reconnection: the agent already has a switch, it just needs
         * a fresh control-plane socket.  We register the new socket
         * without redoing the full MIP.
         */
        g_state.agents.push_back(info);

        std::cout << "[Conductor] Agent reconnected: " << pkt.name() << "\n";
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

        if (info->role == Role::SWITCH_CONN) g_state.remove_switch(info);
        else if (info->role == Role::AGENT_CONN) g_state.remove_agent(info);

        delete info;
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

    for (auto* c : g_state.switches) try_hb(c);
    for (auto* c : g_state.agents)   try_hb(c);
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
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
        // Accept new connections
        while (true) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int client = accept4(listener, (sockaddr*)&peer, &plen, SOCK_NONBLOCK);
            if (client < 0) break;

            int flag = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            auto* info = new ConnInfo();
            info->fd = client;

            if (queue.put_sck(client, info) == nullptr) {
                std::cerr << "[Conductor] Failed to add fd " << client << " to queue\n";
                close(client);
                delete info;
            }
        }

        // Process epoll events
        queue.wait_and_process();

        // Heartbeats
        send_heartbeats(queue);

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

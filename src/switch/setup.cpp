#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <csignal>
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

enum class ConnRole { CONDUCTOR, AGENT_PENDING, AGENT_AUTHENTICATED };

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

        std::cout << "[Switch] Agent " << info->name
                << " authenticated (fd=" << data.fd
                << ", ip=" << ipv4_to_string(entry->virtual_ipv4) << ")\n";
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
        if (info == g_state.conductor) {
            g_state.conductor = nullptr;
        }

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

        // Expire old tokens
        g_state.expire_tokens();

        // Heartbeats
        send_heartbeats(queue);
    }

    close(listener);
    std::cout << "[Switch] Shutting down.\n";
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}

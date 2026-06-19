#include <iostream>
#include <string>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "common/tun.h"
#include "mip.pb.h"
#include "common/config.h"
#include "common/socket_utils.h"
#include "vnet/protocol/dispatch.hpp"
#include "vnet/netqueue/netqueue.hpp"

using namespace vnet::protocol;
using namespace vnet::netqueue;
using clk = std::chrono::steady_clock;

static const std::chrono::seconds HEARTBEAT_INTERVAL {30};
static const int MAX_CONNECT_RETRIES  = 20;
static const int RETRY_DELAY_MS       = 2000;

static volatile sig_atomic_t g_running = 1;
static void on_signal(int) { g_running = 0; }

// ---------------------------------------------------------------------------
//  Connection metadata
// ---------------------------------------------------------------------------

enum class ConnRole { CONDUCTOR, SWITCH };

struct ConnInfo {
    ConnRole role;
    int      fd = -1;
    clk::time_point last_hb_sent = clk::now();
};

struct AgentState {
    ConnInfo* conductor = nullptr;
    ConnInfo* sw        = nullptr;
};
static AgentState g_state;

// ---------------------------------------------------------------------------
//  Dispatch — handles packets arriving via NetQueue
// ---------------------------------------------------------------------------

struct AgentDispatch : public Dispatch {

    void onHeartbeat(socket_data) override {}

    /* 
     * After initial MIP the control-plane is mostly idle.
     * Future extensions (DNS queries, IP group updates) go here.
     */

    void onClose(close_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info) return;

        std::cout << "[Agent] Connection closed: fd=" << data.fd
                  << " role=" << (int)info->role << "\n";

        if (info == g_state.conductor) g_state.conductor = nullptr;
        if (info == g_state.sw)        g_state.sw        = nullptr;

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
    try_hb(g_state.sw);
}

// ---------------------------------------------------------------------------
//  connect_to with retries (waits for the target to be up)
// ---------------------------------------------------------------------------

static int connect_with_retry(const MachineConfig& mc, const char* label) {
    for (int attempt = 1; attempt <= MAX_CONNECT_RETRIES; attempt++) {
        int fd = connect_to(mc);
        if (fd >= 0) return fd;

        std::cerr << "[Agent] " << label << " attempt " << attempt
                  << "/" << MAX_CONNECT_RETRIES << " failed, retrying...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS));
    }
    return -1;
}

// ---------------------------------------------------------------------------
//  Main — runs the sequential MIP then enters the event loop
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (argc != 3) {
        std::cerr << "Usage: ./agent <name> <auth_key>\n";
        return 1;
    }

    std::string agent_name = argv[1];
    std::string auth_key   = argv[2];

    const char* cdt_ip_env   = std::getenv("CONDUCTOR_IP");
    const char* cdt_port_env = std::getenv("CONDUCTOR_PORT");
    if (!cdt_ip_env || !cdt_port_env) {
        std::cerr << "[Agent] CONDUCTOR_IP and CONDUCTOR_PORT must be set\n";
        return 1;
    }

    MachineConfig conductor_cfg {
        cdt_ip_env,
        static_cast<uint16_t>(std::stoi(cdt_port_env))
    };

    // ===================================================================
    //  STEP 1 — Connect to conductor and send Agent MIP
    // ===================================================================
    std::cout << "[Agent] Connecting to conductor "
              << conductor_cfg.ip << ":" << conductor_cfg.port << " ...\n";

    int cdt_sock = connect_with_retry(conductor_cfg, "conductor");
    if (cdt_sock < 0) {
        std::cerr << "[Agent] Could not reach conductor\n";
        return 1;
    }

    mip::PacketAgentMIP mip_pkt;
    mip_pkt.set_name(agent_name);
    mip_pkt.set_auth_key(auth_key);
    mip_pkt.set_network("vnet.internal");

    if (!send_protobuf_packet(cdt_sock, PacketType::AGENT_MIP, mip_pkt)) {
        std::cerr << "[Agent] Failed to send MIP\n";
        close(cdt_sock);
        return 1;
    }

    // ===================================================================
    //  STEP 2 — Receive switch assignment
    // ===================================================================
    mip::PacketConnectToSwitch assignment;
    if (!read_protobuf_packet(cdt_sock, PacketType::CONNECT_TO_SWITCH, assignment)) {
        std::cerr << "[Agent] Did not receive switch assignment\n";
        close(cdt_sock);
        return 1;
    }

    std::string sw_ip   = ipv4_to_string(assignment.switch_ipv4());
    uint16_t    sw_port = static_cast<uint16_t>(assignment.switch_port());

    std::cout << "[Agent] Assigned to switch " << assignment.switch_name()
              << " @ " << sw_ip << ":" << sw_port << "\n";

    // ===================================================================
    //  STEP 3 — Connect to switch and authenticate
    // ===================================================================
    MachineConfig sw_cfg { sw_ip, sw_port };

    int sw_sock = connect_with_retry(sw_cfg, "switch");
    if (sw_sock < 0) {
        std::cerr << "[Agent] Cannot reach assigned switch\n";
        close(cdt_sock);
        return 1;
    }

    mip::PacketAuthConnectToSwitch auth_pkt;
    auth_pkt.set_connection_token(assignment.connection_token());

    if (!send_protobuf_packet(sw_sock, PacketType::AUTH_CONNECT_TO_SWITCH, auth_pkt)) {
        std::cerr << "[Agent] Failed to send auth to switch\n";
        close(sw_sock);
        close(cdt_sock);
        return 1;
    }

    mip::PacketConnectionAccepted ack;
    if (!read_protobuf_packet(sw_sock, PacketType::CONNECTION_ACCEPTED, ack)) {
        std::cerr << "[Agent] Switch did not accept connection\n";
        close(sw_sock);
        close(cdt_sock);
        return 1;
    }

    uint32_t virtual_ipv4 = ack.virtual_ipv4();
    std::cout << "[Agent] Authenticated with switch " << assignment.switch_name()
          << ", virtual IP: " << ipv4_to_string(virtual_ipv4) << "\n";

    // ===================================================================
    //  STEP 4 — Open TUN device with assigned virtual IP
    // ===================================================================
    
    std::string tun_name = "vnet-" + agent_name;
    int tun_fd = tun_open(tun_name, virtual_ipv4);
    if (tun_fd < 0) {
        std::cerr << "[Agent] Failed to open TUN device\n";
        close(sw_sock);
        close(cdt_sock);
        return 1;
    }

    // ===================================================================
    //  STEP 5 — Switch both fds to non-blocking, enter event loop
    // ===================================================================
    set_nonblocking(cdt_sock);
    set_nonblocking(sw_sock);

    int flag = 1;
    setsockopt(cdt_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    setsockopt(sw_sock,  IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    AgentDispatch dispatch;
    NetworkQueueHandler handler = makeNetworkQueueHandler(&dispatch);
    NetQueue queue(handler, /*epoll_timeout_ms=*/200);

    auto* cdt_info = new ConnInfo();
    cdt_info->role = ConnRole::CONDUCTOR;
    cdt_info->fd   = cdt_sock;
    g_state.conductor = cdt_info;
    queue.put_sck(cdt_sock, cdt_info);

    auto* sw_info = new ConnInfo();
    sw_info->role = ConnRole::SWITCH;
    sw_info->fd   = sw_sock;
    g_state.sw = sw_info;
    queue.put_sck(sw_sock, sw_info);

    queue.put_tun(tun_fd, nullptr);

    std::cout << "[Agent] Entering event loop.\n";

    while (g_running) {
        queue.wait_and_process();
        send_heartbeats(queue);
    }

    std::cout << "[Agent] Shutting down.\n";
    tun_close(tun_fd, tun_name);
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}

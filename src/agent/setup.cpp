#include <iostream>
#include <string>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "common/tun.h"
#include "mip.pb.h"
#include "common/config.h"
#include "common/socket_utils.h"
#include "common/sysutil.h"
#include "vnet/protocol/dispatch.hpp"
#include "vnet/netqueue/netqueue.hpp"
#include "vnet/tls/session.hpp"

using namespace vnet::protocol;
using namespace vnet::netqueue;
using clk = std::chrono::steady_clock;

/* A dead peer is noticed within g_dead; both can be set per process. */
static std::chrono::seconds       g_heartbeat{10};
static std::chrono::seconds       g_dead{30};
static std::string g_agent_name;
static std::string g_auth_key;
static std::string g_overlay_cidr = "10.0.0.0/8";
static bool        g_default_via_tun = true;
static bool        g_lockdown = false;
static std::vector<std::string> g_lockdown_allow;
static MachineConfig g_conductor_cfg{"", 0};
static clk::time_point g_last_reconn{};

/* Time between attempts while the conductor or the switch cannot be reached. */
static const int RETRY_DELAY_MS       = 2000;
/* A blocking step of the join (connect, or waiting for one reply). */
static const int STEP_TIMEOUT_MS      = 10000;
/* Tries on the assigned switch before asking the conductor again. */
static const int SWITCH_TRIES         = 3;

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
    clk::time_point last_rx = clk::now();
    /* Conductor rejoin through the queue: not yet connected. */
    bool            connected = true;
    clk::time_point dialed_at{};
};

struct AgentState {
    ConnInfo* conductor = nullptr;
    ConnInfo* sw        = nullptr;
};
static AgentState g_state;

// ---------------------------------------------------------------------------
//  Dispatch: handles packets arriving via NetQueue
// ---------------------------------------------------------------------------

struct AgentDispatch : public Dispatch {
    NetQueue* queue = nullptr;
    int       tun_fd = -1;

    void onHeartbeat(socket_data) override {}

    void onActivity(socket_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (info) info->last_rx = clk::now();
    }

    /* The control socket dialed by maybe_reconnect_conductor is up. */
    void onConnected(socket_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info || info->role != ConnRole::CONDUCTOR) return;
        info->connected = true;
        info->last_rx   = clk::now();
        if (!vnet::tls::peer_has_role(data.net_element->ssl, vnet::tls::ROLE_CONDUCTOR)) {
            std::cerr << "[Agent] The conductor's certificate role is '"
                      << vnet::tls::peer_role(data.net_element->ssl) << "'\n";
            queue->close(data.fd);
            return;
        }

        mip::PacketAgentMRP mrp;
        mrp.set_name(g_agent_name);
        mrp.set_auth_key(g_auth_key);
        queue->send(data.fd, PacketType::AGENT_MRP, mrp);
        std::cout << "[Agent] Control plane reconnected\n";
    }

    /*
     * A full IPv4 packet came off the TUN. Hand it to the switch, whose
     * black box decides where it goes.
     */
    void onTunReady(tun_data data) override {
        if (!queue || !g_state.sw || g_state.sw->fd < 0) return;
        if (data.ip_buffer == nullptr || data.ip_buffer_size < 20) return;
        if ((data.ip_buffer[0] >> 4) != 4) return;

        // A full buffer drops the datagram, as a busy link would.
        queue->send_data(g_state.sw->fd, PacketType::IPV4_RAW,
                         data.ip_buffer, data.ip_buffer_size);
    }

    /*
     * The switch delivered a packet for our virtual IP. One write, the whole
     * datagram: a TUN rejects a partial packet.
     */
    void onIPv4Raw(socket_data data, const uint8_t* raw, size_t len) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info || info->role != ConnRole::SWITCH) return;
        if (tun_fd < 0) return;

        for (;;) {
            ssize_t n = ::write(tun_fd, raw, len);
            if (n < 0 && errno == EINTR) continue;
            if (n != static_cast<ssize_t>(len) && errno != EAGAIN) {
                perror("[Agent] tun write");
            }
            return;
        }
    }

    void onClose(close_data data) override {
        auto* info = static_cast<ConnInfo*>(data.ptr_data);
        if (!info) return;

        std::cout << "[Agent] Connection closed: fd=" << data.fd << " "
                  << (info->role == ConnRole::SWITCH ? "(switch)" : "(conductor)") << "\n";

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
        if (c && c->fd >= 0 && c->connected && now - c->last_hb_sent >= g_heartbeat) {
            queue.send_heartbeat(c->fd);
            c->last_hb_sent = now;
        }
    };

    try_hb(g_state.conductor);
    try_hb(g_state.sw);
}

static void reap_dead(NetQueue& queue) {
    auto now = clk::now();
    std::vector<int> dead;
    auto check = [&](ConnInfo* c) {
        if (!c || c->fd < 0) return;
        if (!c->connected) {
            if (now - c->dialed_at >= std::chrono::milliseconds(STEP_TIMEOUT_MS))
                dead.push_back(c->fd);
            return;
        }
        if (now - c->last_rx >= g_dead)
            dead.push_back(c->fd);
    };
    check(g_state.conductor);
    check(g_state.sw);
    for (int fd : dead) {
        std::cerr << "[Agent] Heartbeat timeout on fd " << fd << "\n";
        queue.close(fd);
    }
}

/*
 * The control socket died while the switch link is fine: dial the
 * conductor again through the queue and send an MRP when it is up. The
 * data path keeps running meanwhile.
 */
static void maybe_reconnect_conductor(NetQueue& queue) {
    if (g_state.conductor) return;
    if (!g_state.sw) return;
    auto now = clk::now();
    if (now - g_last_reconn < std::chrono::milliseconds(RETRY_DELAY_MS)) return;
    g_last_reconn = now;

    int fd = connect_start(g_conductor_cfg.ip, g_conductor_cfg.port);
    if (fd < 0) return;

    auto* info = new ConnInfo();
    info->role      = ConnRole::CONDUCTOR;
    info->fd        = fd;
    info->connected = false;
    info->dialed_at = now;

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
    std::cout << "[Agent] Reconnecting control plane\n";
}

// ---------------------------------------------------------------------------
//  Host routes
// ---------------------------------------------------------------------------

static uint32_t peer_ipv4(int fd) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len) != 0)
        return 0;
    return peer.sin_addr.s_addr;
}

/*
 * The physical default route as it was before the first join, and the
 * /32 pins installed through it. Once the default route points into the
 * TUN, /proc/net/route no longer says where the physical gateway was, so
 * it is read once and kept.
 */
struct HostRoutes {
    bool        saved = false;
    bool        had_default = false;
    std::string iface;
    uint32_t    gateway = 0;
    std::vector<uint32_t> pins;
};
static HostRoutes g_host;

/*
 * The saved route is also written to a file, so an agent that crashed
 * (no chance to restore anything) finds the physical gateway again when
 * it is started next, instead of a machine with no default route.
 */
static std::string route_state_path() {
    return env_or("VNET_STATE_DIR", "/run/vnet") + "/agent-" + g_agent_name + ".route";
}

static void write_route_state() {
    std::string dir = env_or("VNET_STATE_DIR", "/run/vnet");
    run_program({"mkdir", "-p", dir}, "[Agent]", true);
    std::ofstream out(route_state_path(), std::ios::trunc);
    if (!out) return;
    out << ipv4_to_string(g_host.gateway) << " " << g_host.iface << "\n";
}

static bool read_route_state(std::string& iface, uint32_t& gateway) {
    std::ifstream in(route_state_path());
    std::string gw;
    if (!in || !(in >> gw >> iface)) return false;
    gateway = resolve_ipv4(gw);
    return gateway != 0 && valid_name(iface, 15);
}

static void save_physical_default() {
    if (g_host.saved) return;
    g_host.saved = true;
    g_host.had_default = read_default_route(g_host.iface, g_host.gateway);
    if (g_host.had_default && !valid_name(g_host.iface, 15)) {
        std::cerr << "[Agent] Unexpected interface name '" << g_host.iface << "'\n";
        g_host.had_default = false;
    }

    // No default route, or one left in a TUN by an earlier agent that
    // died: use what that agent saved.
    bool left_by_agent = g_host.had_default && g_host.iface.rfind("vnet-", 0) == 0;
    if (!g_host.had_default || left_by_agent) {
        std::string iface;
        uint32_t gateway = 0;
        if (read_route_state(iface, gateway)) {
            g_host.iface = iface;
            g_host.gateway = gateway;
            g_host.had_default = true;
            run_program({"ip", "route", "replace", "default", "via",
                         ipv4_to_string(gateway), "dev", iface}, "[Agent]");
            std::cout << "[Agent] Restored the default route a previous agent "
                      << "left behind: via " << ipv4_to_string(gateway)
                      << " dev " << iface << "\n";
        } else if (left_by_agent) {
            g_host.had_default = false;
        }
    }

    if (g_host.had_default)
        write_route_state();
    else
        std::cerr << "[Agent] No default route; control-plane pins are skipped\n";
}

static void pin_route(uint32_t ip) {
    if (ip == 0 || !g_host.had_default) return;
    for (uint32_t p : g_host.pins) if (p == ip) return;
    if (run_program({"ip", "route", "replace", ipv4_to_string(ip) + "/32",
                     "via", ipv4_to_string(g_host.gateway), "dev", g_host.iface}, "[Agent]"))
        g_host.pins.push_back(ip);
}

/*
 * The TUN is a /32. Other virtual IPs are reached by an explicit overlay
 * route into the device. Conductor and switch addresses stay on the
 * physical default route so the control sockets are not captured by the overlay.
 */
static void install_host_routes(const std::string& tun_name,
                                uint32_t conductor_ip,
                                uint32_t switch_ip) {
    save_physical_default();
    pin_route(conductor_ip);
    pin_route(switch_ip);

    run_program({"ip", "route", "replace", g_overlay_cidr, "dev", tun_name}, "[Agent]");

    if (g_default_via_tun && g_host.had_default)
        run_program({"ip", "route", "replace", "default", "dev", tun_name}, "[Agent]");
}

/* Put the machine's routing back the way it was before the first join. */
static void restore_host_routes() {
    if (!g_host.saved) return;
    if (g_default_via_tun && g_host.had_default) {
        run_program({"ip", "route", "replace", "default", "via",
                     ipv4_to_string(g_host.gateway), "dev", g_host.iface}, "[Agent]");
    }
    for (uint32_t ip : g_host.pins)
        run_program({"ip", "route", "del", ipv4_to_string(ip) + "/32"}, "[Agent]", true);
    g_host.pins.clear();
    unlink(route_state_path().c_str());
}

// ---------------------------------------------------------------------------
//  Lockdown: the host firewall of a contestant machine
//
//  The overlay only sees what enters the TUN. Without these rules a
//  contestant still reaches the physical LAN and, over IPv6, the internet.
//  With VNET_LOCKDOWN=1 the machine may talk only to the conductor, its
//  switch, the TUN, DHCP, and the networks in VNET_LOCKDOWN_ALLOW
//  (for the admins' SSH, say). IPv6 is blocked entirely.
// ---------------------------------------------------------------------------

static const char* CHAIN_IN  = "VNET_IN";
static const char* CHAIN_OUT = "VNET_OUT";
static bool g_lockdown_applied = false;

static bool ipt(const std::vector<std::string>& args, bool quiet = false) {
    std::vector<std::string> argv = {"iptables", "-w", "5"};
    argv.insert(argv.end(), args.begin(), args.end());
    return run_program(argv, "[Agent]", quiet);
}
static bool ip6t(const std::vector<std::string>& args, bool quiet = false) {
    std::vector<std::string> argv = {"ip6tables", "-w", "5"};
    argv.insert(argv.end(), args.begin(), args.end());
    return run_program(argv, "[Agent]", quiet);
}

/* Make the chains and hook them first in INPUT and OUTPUT, once. */
static void lockdown_hook() {
    for (const char* chain : {CHAIN_IN, CHAIN_OUT}) {
        ipt({"-N", chain}, true);
        ip6t({"-N", chain}, true);
    }
    if (!ipt({"-C", "INPUT", "-j", CHAIN_IN}, true))   ipt({"-I", "INPUT", "1", "-j", CHAIN_IN});
    if (!ipt({"-C", "OUTPUT", "-j", CHAIN_OUT}, true)) ipt({"-I", "OUTPUT", "1", "-j", CHAIN_OUT});
    if (!ip6t({"-C", "INPUT", "-j", CHAIN_IN}, true))   ip6t({"-I", "INPUT", "1", "-j", CHAIN_IN});
    if (!ip6t({"-C", "OUTPUT", "-j", CHAIN_OUT}, true)) ip6t({"-I", "OUTPUT", "1", "-j", CHAIN_OUT});
}

/*
 * Fill the chains for the current conductor and switch. Called on every
 * join, since the switch (and its address) can change.
 */
static void lockdown_apply(const std::string& tun_name,
                           uint32_t conductor_ip, uint16_t conductor_port,
                           uint32_t switch_ip, uint16_t switch_port) {
    lockdown_hook();
    ipt({"-F", CHAIN_IN});
    ipt({"-F", CHAIN_OUT});
    ip6t({"-F", CHAIN_IN});
    ip6t({"-F", CHAIN_OUT});

    // Outbound: loopback, the overlay, the two control links, replies to
    // allowed inbound connections, DHCP, the allowed networks.
    ipt({"-A", CHAIN_OUT, "-o", "lo", "-j", "ACCEPT"});
    ipt({"-A", CHAIN_OUT, "-o", tun_name, "-j", "ACCEPT"});
    if (conductor_ip)
        ipt({"-A", CHAIN_OUT, "-p", "tcp", "-d", ipv4_to_string(conductor_ip),
             "--dport", std::to_string(conductor_port), "-j", "ACCEPT"});
    if (switch_ip)
        ipt({"-A", CHAIN_OUT, "-p", "tcp", "-d", ipv4_to_string(switch_ip),
             "--dport", std::to_string(switch_port), "-j", "ACCEPT"});
    ipt({"-A", CHAIN_OUT, "-m", "conntrack", "--ctstate", "ESTABLISHED,RELATED", "-j", "ACCEPT"});
    ipt({"-A", CHAIN_OUT, "-p", "udp", "--sport", "68", "--dport", "67", "-j", "ACCEPT"});
    for (const auto& cidr : g_lockdown_allow)
        ipt({"-A", CHAIN_OUT, "-d", cidr, "-j", "ACCEPT"});
    ipt({"-A", CHAIN_OUT, "-j", "DROP"});

    // Inbound: loopback, the overlay, replies, DHCP, the allowed networks.
    ipt({"-A", CHAIN_IN, "-i", "lo", "-j", "ACCEPT"});
    ipt({"-A", CHAIN_IN, "-i", tun_name, "-j", "ACCEPT"});
    ipt({"-A", CHAIN_IN, "-m", "conntrack", "--ctstate", "ESTABLISHED,RELATED", "-j", "ACCEPT"});
    ipt({"-A", CHAIN_IN, "-p", "udp", "--sport", "67", "--dport", "68", "-j", "ACCEPT"});
    for (const auto& cidr : g_lockdown_allow)
        ipt({"-A", CHAIN_IN, "-s", cidr, "-j", "ACCEPT"});
    ipt({"-A", CHAIN_IN, "-j", "DROP"});

    // IPv6: nothing but loopback. The overlay carries IPv4 only.
    ip6t({"-A", CHAIN_OUT, "-o", "lo", "-j", "ACCEPT"});
    ip6t({"-A", CHAIN_OUT, "-j", "DROP"});
    ip6t({"-A", CHAIN_IN, "-i", "lo", "-j", "ACCEPT"});
    ip6t({"-A", CHAIN_IN, "-j", "DROP"});

    g_lockdown_applied = true;
    std::cout << "[Agent] Lockdown on: only the conductor, the switch, the overlay"
              << (g_lockdown_allow.empty() ? "" : " and the allowed networks")
              << " are reachable; IPv6 is blocked\n";
}

/*
 * Open the way to a control address before connecting to it while the
 * chains from an earlier join are in force; the next lockdown_apply
 * rewrites the chains from scratch.
 */
static void lockdown_allow_tcp(uint32_t ip, uint16_t port) {
    if (!g_lockdown_applied || ip == 0) return;
    ipt({"-I", CHAIN_OUT, "1", "-p", "tcp", "-d", ipv4_to_string(ip),
         "--dport", std::to_string(port), "-j", "ACCEPT"});
}

static void lockdown_remove() {
    if (!g_lockdown_applied) return;
    ipt({"-D", "INPUT", "-j", CHAIN_IN}, true);
    ipt({"-D", "OUTPUT", "-j", CHAIN_OUT}, true);
    ip6t({"-D", "INPUT", "-j", CHAIN_IN}, true);
    ip6t({"-D", "OUTPUT", "-j", CHAIN_OUT}, true);
    for (const char* chain : {CHAIN_IN, CHAIN_OUT}) {
        ipt({"-F", chain}, true);
        ipt({"-X", chain}, true);
        ip6t({"-F", chain}, true);
        ip6t({"-X", chain}, true);
    }
    g_lockdown_applied = false;
    std::cout << "[Agent] Lockdown removed\n";
}

// ---------------------------------------------------------------------------
//  Join: register with the conductor, then authenticate to the switch
// ---------------------------------------------------------------------------

/* Blocking connect bounded by STEP_TIMEOUT_MS; returns a blocking fd. */
static int connect_bounded(const std::string& host, uint16_t port) {
    int fd = connect_start(host, port);
    if (fd < 0) return -1;

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int rc = poll(&pfd, 1, STEP_TIMEOUT_MS);
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (rc <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
        ::close(fd);
        return -1;
    }
    set_blocking(fd);
    set_socket_timeout(fd, STEP_TIMEOUT_MS);
    return fd;
}

struct Session {
    int      conductor_fd = -1;
    int      switch_fd    = -1;
    ssl_st*  conductor_ssl = nullptr;
    ssl_st*  switch_ssl    = nullptr;
    uint32_t virtual_ipv4  = 0;
    uint16_t switch_port   = 0;
    std::string switch_name;
};

static void close_session(Session& s) {
    vnet::tls::free_session(s.conductor_ssl);
    vnet::tls::free_session(s.switch_ssl);
    if (s.conductor_fd >= 0) ::close(s.conductor_fd);
    if (s.switch_fd >= 0) ::close(s.switch_fd);
    s = Session{};
}

static void wait_retry() {
    for (int waited = 0; waited < RETRY_DELAY_MS && g_running; waited += 100)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

/*
 * One attempt at the whole procedure. Returns true with a filled session;
 * false to try again. Each blocking step is bounded, so a conductor or
 * switch that hangs cannot hold the agent forever.
 */
static bool try_join(Session& s) {
    // Once the default route points into the TUN, a new control address
    // is reachable only through a pin, so pin before connecting.
    save_physical_default();
    uint32_t conductor_ip = resolve_ipv4(g_conductor_cfg.ip);
    pin_route(conductor_ip);
    lockdown_allow_tcp(conductor_ip, g_conductor_cfg.port);

    s.conductor_fd = connect_bounded(g_conductor_cfg.ip, g_conductor_cfg.port);
    if (s.conductor_fd < 0) {
        std::cerr << "[Agent] Conductor " << g_conductor_cfg.ip << ":"
                  << g_conductor_cfg.port << " not reachable\n";
        return false;
    }
    if (vnet::tls::enabled()) {
        s.conductor_ssl = vnet::tls::handshake_client(s.conductor_fd);
        if (!s.conductor_ssl) { std::cerr << "[Agent] TLS to conductor failed\n"; return false; }
        if (!vnet::tls::peer_has_role(s.conductor_ssl, vnet::tls::ROLE_CONDUCTOR)) {
            std::cerr << "[Agent] The conductor's certificate role is '"
                      << vnet::tls::peer_role(s.conductor_ssl) << "'\n";
            return false;
        }
    }

    mip::PacketAgentMIP mip_pkt;
    mip_pkt.set_name(g_agent_name);
    mip_pkt.set_auth_key(g_auth_key);
    mip_pkt.set_network(env_or("VNET_NETWORK", "vnet.internal"));
    if (!send_protobuf_packet(s.conductor_fd, PacketType::AGENT_MIP, mip_pkt, s.conductor_ssl)) {
        std::cerr << "[Agent] Failed to send MIP\n";
        return false;
    }

    mip::PacketConnectToSwitch assignment;
    if (!read_protobuf_packet(s.conductor_fd, PacketType::CONNECT_TO_SWITCH, assignment, s.conductor_ssl)) {
        std::cerr << "[Agent] No switch assignment from the conductor "
                  << "(rejected, no switch available, or no answer)\n";
        return false;
    }

    std::string sw_ip   = ipv4_to_string(assignment.switch_ipv4());
    uint16_t    sw_port = static_cast<uint16_t>(assignment.switch_port());
    s.switch_name = assignment.switch_name();
    s.switch_port = sw_port;
    std::cout << "[Agent] Assigned to switch " << s.switch_name
              << " @ " << sw_ip << ":" << sw_port << "\n";
    pin_route(assignment.switch_ipv4());
    lockdown_allow_tcp(assignment.switch_ipv4(), sw_port);

    for (int attempt = 1; attempt <= SWITCH_TRIES && g_running; attempt++) {
        s.switch_fd = connect_bounded(sw_ip, sw_port);
        if (s.switch_fd >= 0) break;
        std::cerr << "[Agent] Switch " << s.switch_name << " not reachable ("
                  << attempt << "/" << SWITCH_TRIES << ")\n";
        wait_retry();
    }
    if (s.switch_fd < 0) return false;

    if (vnet::tls::enabled()) {
        s.switch_ssl = vnet::tls::handshake_client(s.switch_fd);
        if (!s.switch_ssl) { std::cerr << "[Agent] TLS to switch failed\n"; return false; }
        if (!vnet::tls::peer_has_role(s.switch_ssl, vnet::tls::ROLE_SWITCH)) {
            std::cerr << "[Agent] The switch's certificate role is '"
                      << vnet::tls::peer_role(s.switch_ssl) << "'\n";
            return false;
        }
    }

    mip::PacketAuthConnectToSwitch auth_pkt;
    auth_pkt.set_connection_token(assignment.connection_token());
    if (!send_protobuf_packet(s.switch_fd, PacketType::AUTH_CONNECT_TO_SWITCH, auth_pkt, s.switch_ssl)) {
        std::cerr << "[Agent] Failed to send auth to switch\n";
        return false;
    }

    mip::PacketConnectionAccepted ack;
    if (!read_protobuf_packet(s.switch_fd, PacketType::CONNECTION_ACCEPTED, ack, s.switch_ssl)) {
        std::cerr << "[Agent] Switch did not accept the connection\n";
        return false;
    }
    s.virtual_ipv4 = ack.virtual_ipv4();
    std::cout << "[Agent] Authenticated with switch " << s.switch_name
              << ", virtual IP: " << ipv4_to_string(s.virtual_ipv4) << "\n";

    // Back to non-blocking for the event loop.
    set_socket_timeout(s.conductor_fd, 0);
    set_socket_timeout(s.switch_fd, 0);
    set_nonblocking(s.conductor_fd);
    set_nonblocking(s.switch_fd);
    return true;
}

/* Keep trying until it works or we are told to stop. */
static bool join(Session& s) {
    while (g_running) {
        if (try_join(s)) return true;
        close_session(s);
        wait_retry();
    }
    return false;
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

    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: ./agent <name> [auth_key]\n"
                  << "The key is read from VNET_AUTH_KEY or VNET_AUTH_KEY_FILE "
                  << "when it is not given.\n";
        return 1;
    }

    long hb_sec = g_heartbeat.count(), dead_sec = g_dead.count();
    long cdt_port = 0;
    if (!env_int("VNET_HEARTBEAT_SEC", 1, 3600, hb_sec) ||
        !env_int("VNET_DEAD_SEC", 1, 86400, dead_sec) ||
        !env_int("CONDUCTOR_PORT", 1, 65535, cdt_port))
        return 1;
    g_heartbeat = std::chrono::seconds(hb_sec);
    g_dead      = std::chrono::seconds(dead_sec);
    if (g_dead <= g_heartbeat) {
        std::cerr << "[Agent] VNET_DEAD_SEC must be larger than VNET_HEARTBEAT_SEC\n";
        return 1;
    }
    if (!vnet::tls::enabled() && env_or("VNET_ALLOW_INSECURE", "0") != "1") {
        std::cerr << "[Agent] TLS is off: keys would cross the network in the clear. "
                  << "Set VNET_TLS=1, or VNET_ALLOW_INSECURE=1 for a lab.\n";
        return 1;
    }

    g_agent_name = argv[1];
    if (!valid_name(g_agent_name, AGENT_NAME_MAX)) {
        std::cerr << "[Agent] Name must be 1 to " << AGENT_NAME_MAX
                  << " characters from [A-Za-z0-9_-]\n";
        return 1;
    }
    g_auth_key = load_auth_key(argc == 3 ? argv[2] : nullptr, "[Agent]");
    if (g_auth_key.empty()) {
        std::cerr << "[Agent] No auth key: set VNET_AUTH_KEY or VNET_AUTH_KEY_FILE\n";
        return 1;
    }

    const char* cdt_ip_env = std::getenv("CONDUCTOR_IP");
    if (!cdt_ip_env || !*cdt_ip_env || cdt_port == 0) {
        std::cerr << "[Agent] CONDUCTOR_IP and CONDUCTOR_PORT must be set\n";
        return 1;
    }

    g_overlay_cidr = env_or("VNET_OVERLAY_CIDR", g_overlay_cidr);
    {
        uint32_t addr = 0;
        int prefix = 0;
        if (!parse_cidr(g_overlay_cidr, addr, prefix)) {
            std::cerr << "[Agent] VNET_OVERLAY_CIDR is not a.b.c.d/n: "
                      << g_overlay_cidr << "\n";
            return 1;
        }
    }
    g_default_via_tun = env_or("VNET_DEFAULT_VIA_TUN", "1") != "0";
    g_lockdown        = env_or("VNET_LOCKDOWN", "0") == "1";
    {
        // Comma-separated networks that stay reachable under lockdown.
        std::string allow = env_or("VNET_LOCKDOWN_ALLOW", "");
        size_t start = 0;
        while (start <= allow.size() && !allow.empty()) {
            size_t comma = allow.find(',', start);
            std::string item = allow.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
            if (a != std::string::npos) {
                item = item.substr(a, b - a + 1);
                uint32_t addr = 0;
                int prefix = 0;
                if (!parse_cidr(item, addr, prefix)) {
                    std::cerr << "[Agent] VNET_LOCKDOWN_ALLOW entry is not a.b.c.d/n: " << item << "\n";
                    return 1;
                }
                g_lockdown_allow.push_back(item);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    g_conductor_cfg = MachineConfig{ cdt_ip_env, static_cast<uint16_t>(cdt_port) };

    AgentDispatch dispatch;
    NetworkQueueHandler handler = makeNetworkQueueHandler(&dispatch);
    NetQueue queue(handler, /*epoll_timeout_ms=*/200);
    dispatch.queue = &queue;

    const std::string tun_name = "vnet-" + g_agent_name;
    int      tun_fd  = -1;
    uint32_t tun_ip  = 0;
    int      status  = 0;

    // Each pass registers, runs until the switch link is lost, and
    // registers again. The TUN stays up across passes, so applications
    // keep their interface while the agent finds a new switch.
    while (g_running) {
        Session s;
        std::cout << "[Agent] Connecting to conductor "
                  << g_conductor_cfg.ip << ":" << g_conductor_cfg.port << " ...\n";
        if (!join(s)) break;

        if (tun_fd >= 0 && tun_ip != s.virtual_ipv4) {
            std::cout << "[Agent] Virtual IP changed; reopening the TUN\n";
            queue.close(tun_fd);
            tun_close(-1, tun_name);
            tun_fd = -1;
        }
        if (tun_fd < 0) {
            tun_fd = tun_open(tun_name, s.virtual_ipv4, 32);
            if (tun_fd < 0) {
                std::cerr << "[Agent] Failed to open TUN device\n";
                close_session(s);
                status = 1;
                break;
            }
            tun_ip = s.virtual_ipv4;
            dispatch.tun_fd = tun_fd;
            if (queue.put_tun(tun_fd, nullptr) == nullptr) {
                std::cerr << "[Agent] Failed to add the TUN to the queue\n";
                status = 1;
                break;
            }
        }

        install_host_routes(tun_name, peer_ipv4(s.conductor_fd), peer_ipv4(s.switch_fd));
        if (g_lockdown)
            lockdown_apply(tun_name, peer_ipv4(s.conductor_fd), g_conductor_cfg.port,
                           peer_ipv4(s.switch_fd), s.switch_port);

        auto* cdt_info = new ConnInfo();
        cdt_info->role = ConnRole::CONDUCTOR;
        cdt_info->fd   = s.conductor_fd;
        if (queue.put_sck(s.conductor_fd, cdt_info) == nullptr) {
            std::cerr << "[Agent] Failed to add the conductor socket to the queue\n";
            delete cdt_info;
            close_session(s);
            wait_retry();
            continue;
        }
        g_state.conductor = cdt_info;
        if (s.conductor_ssl) queue.adopt_tls(s.conductor_fd, s.conductor_ssl);

        auto* sw_info = new ConnInfo();
        sw_info->role = ConnRole::SWITCH;
        sw_info->fd   = s.switch_fd;
        if (queue.put_sck(s.switch_fd, sw_info) == nullptr) {
            std::cerr << "[Agent] Failed to add the switch socket to the queue\n";
            delete sw_info;
            queue.close(s.conductor_fd);
            vnet::tls::free_session(s.switch_ssl);
            ::close(s.switch_fd);
            wait_retry();
            continue;
        }
        g_state.sw = sw_info;
        if (s.switch_ssl) queue.adopt_tls(s.switch_fd, s.switch_ssl);

        std::cout << "[Agent] Entering event loop.\n";

        while (g_running && g_state.sw) {
            queue.wait_and_process();
            maybe_reconnect_conductor(queue);
            send_heartbeats(queue);
            reap_dead(queue);
        }

        if (!g_running) break;

        // The switch is gone. The conductor must see this agent leave
        // before it can assign it again, so the control socket goes too.
        std::cerr << "[Agent] Switch link lost; joining again\n";
        if (g_state.conductor) queue.close(g_state.conductor->fd);
        g_state.conductor = nullptr;
        wait_retry();
    }

    std::cout << "[Agent] Shutting down.\n";
    if (g_state.conductor) queue.close(g_state.conductor->fd);
    if (g_state.sw)        queue.close(g_state.sw->fd);
    if (tun_fd >= 0) {
        queue.close(tun_fd);
        tun_close(-1, tun_name);
    }
    lockdown_remove();
    restore_host_routes();
    google::protobuf::ShutdownProtobufLibrary();
    return status;
}

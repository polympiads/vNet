#include "netqueue/netqueue_macro.hpp"
#include "netqueue/netqueue_test_handler.hpp"
#include "vnet/tls/session.hpp"
#include "common/sysutil.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>

using namespace vnet::netqueue;
using namespace vnet::protocol;

static std::vector<socket_data> connected_datas;
static void onConnectedTest(void*, socket_data data) {
    connected_datas.push_back(data);
}

static NetworkQueueHandler handlerWithConnected() {
    NetworkQueueHandler handler = createTestHandler();
    handler.onConnected = &onConnectedTest;
    return handler;
}

static void reset_records() {
    socket_datas.clear();
    close_datas.clear();
    tun_datas.clear();
    connected_datas.clear();
}

/* A listening TCP socket on 127.0.0.1 with a kernel-chosen port. */
static int listen_local(uint16_t& port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(bind(fd, (sockaddr*)&addr, sizeof(addr)), 0);
    EXPECT_EQ(listen(fd, 8), 0);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(getsockname(fd, (sockaddr*)&addr, &len), 0);
    port = ntohs(addr.sin_port);
    return fd;
}

static int start_connect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    int rc = connect(fd, (sockaddr*)&addr, sizeof(addr));
    EXPECT_TRUE(rc == 0 || errno == EINPROGRESS);
    return fd;
}

TEST(NetQueueConnect, PlainConnectFiresOnConnected) {
    reset_records();
    uint16_t port = 0;
    int listener = listen_local(port);

    NetQueue* queue = new NetQueue(handlerWithConnected(), 100);
    int client = start_connect(port);
    NetworkElement* element = queue->put_sck_connecting(client, (void*) 0x1, nullptr);
    ASSERT_NE(element, nullptr);
    EXPECT_EQ(element->state, SCK_CONNECTING);

    int server = -1;
    for (int i = 0; i < 50 && connected_datas.empty(); i++) {
        if (server < 0) server = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK);
        queue->wait_and_process();
    }
    ASSERT_GE(server, 0);
    ASSERT_EQ(connected_datas.size(), 1u);
    EXPECT_EQ(connected_datas[0].fd, client);
    EXPECT_EQ(connected_datas[0].ptr_data, (void*) 0x1);
    EXPECT_EQ(element->state, SCK_HEADER);

    // Packets flow after the connect.
    PacketHeader header(0, (PacketType) 0x32);
    EXPECT_EQ(write(server, &header, sizeof(header)), (ssize_t) sizeof(header));
    for (int i = 0; i < 50 && socket_datas.empty(); i++) queue->wait_and_process();
    ASSERT_EQ(socket_datas.size(), 1u);
    EXPECT_EQ(socket_datas[0].packet_type, (PacketType) 0x32);

    close(server);
    close(listener);
    delete queue;
}

TEST(NetQueueConnect, RefusedConnectCloses) {
    reset_records();
    uint16_t port = 0;
    int listener = listen_local(port);
    close(listener);   // nothing listens on port any more

    NetQueue* queue = new NetQueue(handlerWithConnected(), 100);
    int client = start_connect(port);
    NetworkElement* element = queue->put_sck_connecting(client, nullptr, nullptr);
    ASSERT_NE(element, nullptr);

    for (int i = 0; i < 50 && close_datas.empty(); i++) queue->wait_and_process();
    ASSERT_EQ(close_datas.size(), 1u);
    EXPECT_EQ(close_datas[0].fd, client);
    EXPECT_TRUE(connected_datas.empty());
    EXPECT_EQ(queue->get_network_element_from_fd(client), nullptr);

    delete queue;
}

/* Self-signed certificate for the TLS tests, made once with the openssl tool. */
static bool make_test_cert(std::string& cert, std::string& key) {
    std::string base = "/tmp/vnet_nq_tls_" + std::to_string(getpid());
    cert = base + "_cert.pem";
    key  = base + "_key.pem";
    return run_program({"openssl", "req", "-x509", "-newkey", "rsa:2048",
                        "-keyout", key, "-out", cert, "-days", "2", "-nodes",
                        "-subj", "/CN=vnet-test"}, "[Test]", true);
}

class NetQueueTls : public ::testing::Test {
protected:
    std::string cert, key;
    void SetUp() override {
        // OpenSSL writes an alert to a peer that hung up; the daemons
        // ignore SIGPIPE for the same reason.
        signal(SIGPIPE, SIG_IGN);
        ASSERT_TRUE(make_test_cert(cert, key));
        setenv("VNET_TLS", "1", 1);
        setenv("VNET_TLS_CERT", cert.c_str(), 1);
        setenv("VNET_TLS_KEY", key.c_str(), 1);
        setenv("VNET_TLS_CA", cert.c_str(), 1);
        ASSERT_TRUE(vnet::tls::init());
        ASSERT_TRUE(vnet::tls::enabled());
        reset_records();
    }
    void TearDown() override {
        unlink(cert.c_str());
        unlink(key.c_str());
    }
};

TEST_F(NetQueueTls, HandshakeCompletesWithoutBlocking) {
    PREPARE_SOCKET_PAIR
    NetQueue* queue = new NetQueue(handlerWithConnected(), 100);

    ssl_st* server_ssl = vnet::tls::new_session(fd_server);
    ssl_st* client_ssl = vnet::tls::new_session(fd_client);
    ASSERT_NE(server_ssl, nullptr);
    ASSERT_NE(client_ssl, nullptr);

    NetworkElement* server = queue->put_sck_tls_server(fd_server, (void*) 0x5, server_ssl);
    NetworkElement* client = queue->put_sck_tls_client(fd_client, (void*) 0xC, client_ssl);
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);
    EXPECT_EQ(server->state, SCK_TLS_ACCEPT);
    EXPECT_EQ(client->state, SCK_TLS_CONNECT);

    for (int i = 0; i < 100 && connected_datas.size() < 2; i++) queue->wait_and_process();
    ASSERT_EQ(connected_datas.size(), 2u);
    EXPECT_EQ(server->state, SCK_HEADER);
    EXPECT_EQ(client->state, SCK_HEADER);
    EXPECT_TRUE(close_datas.empty());

    // A packet from the client reaches the server through the session.
    PacketHeader header(3, (PacketType) 0x32);
    uint8_t frame[9];
    memcpy(frame, &header, 6);
    memcpy(frame + 6, "abc", 3);
    EXPECT_TRUE(queue->send(fd_client, frame, sizeof(frame)));
    for (int i = 0; i < 100 && socket_datas.empty(); i++) queue->wait_and_process();
    ASSERT_EQ(socket_datas.size(), 1u);
    EXPECT_EQ(socket_datas[0].fd, fd_server);
    EXPECT_EQ(socket_datas[0].payload_size, 3u);
    EXPECT_EQ(memcmp(socket_datas[0].packet_buffer, "abc", 3), 0);

    delete queue;
}

TEST_F(NetQueueTls, SilentPeerDoesNotStallOtherSockets) {
    PREPARE_SOCKET_PAIR
    NetQueue* queue = new NetQueue(handlerWithConnected(), 100);

    // A peer that connected and then says nothing.
    ssl_st* server_ssl = vnet::tls::new_session(fd_server);
    NetworkElement* silent = queue->put_sck_tls_server(fd_server, nullptr, server_ssl);
    ASSERT_NE(silent, nullptr);

    // A plain socket beside it still gets its packets.
    int sv2[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv2), 0);
    queue->put_sck(sv2[0], nullptr);
    PacketHeader header(0, (PacketType) 0x32);
    EXPECT_EQ(write(sv2[1], &header, sizeof(header)), (ssize_t) sizeof(header));

    for (int i = 0; i < 20 && socket_datas.empty(); i++) queue->wait_and_process();
    EXPECT_EQ(socket_datas.size(), 1u);
    EXPECT_EQ(silent->state, SCK_TLS_ACCEPT);

    // The silent peer hanging up is a hangup, not a TLS fault.
    close(fd_client);
    for (int i = 0; i < 20 && close_datas.empty(); i++) queue->wait_and_process();
    ASSERT_EQ(close_datas.size(), 1u);
    EXPECT_EQ(close_datas[0].fd, fd_server);

    close(sv2[1]);
    delete queue;
}

TEST_F(NetQueueTls, PlaintextPeerFailsHandshake) {
    PREPARE_SOCKET_PAIR
    NetQueue* queue = new NetQueue(handlerWithConnected(), 100);

    ssl_st* server_ssl = vnet::tls::new_session(fd_server);
    NetworkElement* server = queue->put_sck_tls_server(fd_server, nullptr, server_ssl);
    ASSERT_NE(server, nullptr);

    // Garbage instead of a ClientHello.
    const char junk[] = "GET / HTTP/1.0\r\n\r\n";
    EXPECT_EQ(write(fd_client, junk, sizeof(junk)), (ssize_t) sizeof(junk));
    for (int i = 0; i < 20 && close_datas.empty(); i++) queue->wait_and_process();
    ASSERT_EQ(close_datas.size(), 1u);
    EXPECT_EQ(close_datas[0].fd, fd_server);
    EXPECT_TRUE(connected_datas.empty());

    close(fd_client);
    delete queue;
}

/*
 * A dial that fails while it still holds an unused TLS session must not
 * disturb the other TLS links in the queue.
 */
TEST_F(NetQueueTls, FailedTlsDialLeavesOtherLinksAlone) {
    PREPARE_SOCKET_PAIR
    NetQueue* queue = new NetQueue(handlerWithConnected(), 100);

    ssl_st* server_ssl = vnet::tls::new_session(fd_server);
    ssl_st* client_ssl = vnet::tls::new_session(fd_client);
    ASSERT_NE(queue->put_sck_tls_server(fd_server, nullptr, server_ssl), nullptr);
    ASSERT_NE(queue->put_sck_tls_client(fd_client, nullptr, client_ssl), nullptr);
    for (int i = 0; i < 100 && connected_datas.size() < 2; i++) queue->wait_and_process();
    ASSERT_EQ(connected_datas.size(), 2u);

    // A dial to a port nobody listens on, with a session attached.
    uint16_t port = 0;
    int listener = listen_local(port);
    close(listener);
    int dial = start_connect(port);
    ssl_st* dial_ssl = vnet::tls::new_session(dial);
    ASSERT_NE(queue->put_sck_connecting(dial, (void*) 0xD, dial_ssl), nullptr);
    for (int i = 0; i < 50 && close_datas.empty(); i++) queue->wait_and_process();
    ASSERT_EQ(close_datas.size(), 1u);
    EXPECT_EQ(close_datas[0].fd, dial);

    // The TLS link still carries packets both ways.
    PacketHeader header(0, (PacketType) 0x32);
    EXPECT_TRUE(queue->send(fd_client, &header, sizeof(header)));
    EXPECT_TRUE(queue->send(fd_server, &header, sizeof(header)));
    for (int i = 0; i < 100 && socket_datas.size() < 2; i++) queue->wait_and_process();
    EXPECT_EQ(socket_datas.size(), 2u);
    EXPECT_EQ(close_datas.size(), 1u);

    delete queue;
}

/*
 * A TLS peer that vanishes without close_notify leaves an error in the
 * OpenSSL error queue. That must be reported as a hangup on that socket,
 * and the next read on a healthy TLS socket must not inherit it.
 */
TEST_F(NetQueueTls, VanishedPeerDoesNotPoisonOtherLinks) {
    NetQueue* queue = new NetQueue(handlerWithConnected(), 100);

    int a[2], b[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, a), 0);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, b), 0);
    ASSERT_NE(queue->put_sck_tls_server(a[0], (void*) 0xA0, vnet::tls::new_session(a[0])), nullptr);
    ASSERT_NE(queue->put_sck_tls_client(a[1], (void*) 0xA1, vnet::tls::new_session(a[1])), nullptr);
    ASSERT_NE(queue->put_sck_tls_server(b[0], (void*) 0xB0, vnet::tls::new_session(b[0])), nullptr);
    ASSERT_NE(queue->put_sck_tls_client(b[1], (void*) 0xB1, vnet::tls::new_session(b[1])), nullptr);
    for (int i = 0; i < 200 && connected_datas.size() < 4; i++) queue->wait_and_process();
    ASSERT_EQ(connected_datas.size(), 4u);

    // Pair A: the server side is torn down like a killed process (no
    // close_notify); the client side sees an unexpected EOF.
    queue->close(a[0]);
    ASSERT_EQ(close_datas.size(), 1u);
    for (int i = 0; i < 50 && close_datas.size() < 2; i++) queue->wait_and_process();
    ASSERT_EQ(close_datas.size(), 2u);
    EXPECT_EQ(close_datas[1].fd, a[1]);
    EXPECT_EQ(close_datas[1].reason, CLOSE_HANGUP);

    // What a switch does next: dial the conductor again, which fails.
    uint16_t port = 0;
    int listener = listen_local(port);
    close(listener);
    int dial = start_connect(port);
    ASSERT_NE(queue->put_sck_connecting(dial, (void*) 0xD, vnet::tls::new_session(dial)), nullptr);
    for (int i = 0; i < 50 && close_datas.size() < 3; i++) queue->wait_and_process();
    ASSERT_EQ(close_datas.size(), 3u);

    // Pair B still carries packets both ways, several times over.
    PacketHeader header(0, (PacketType) 0x32);
    for (int round = 0; round < 3; round++) {
        size_t before = socket_datas.size();
        EXPECT_TRUE(queue->send(b[0], &header, sizeof(header)));
        EXPECT_TRUE(queue->send(b[1], &header, sizeof(header)));
        for (int i = 0; i < 100 && socket_datas.size() < before + 2; i++) queue->wait_and_process();
        EXPECT_EQ(socket_datas.size(), before + 2) << "round " << round;
    }
    EXPECT_EQ(close_datas.size(), 3u);

    delete queue;
}

/*
 * Over TLS the packets beyond the cap sit inside the session, where epoll
 * does not see them; the queue must come back for them on its own.
 */
TEST_F(NetQueueTls, BacklogInsideTheSessionIsDrained) {
    PREPARE_SOCKET_PAIR
    NetQueue* queue = new NetQueue(handlerWithConnected(), 100);
    ASSERT_NE(queue->put_sck_tls_server(fd_server, (void*) 0x5, vnet::tls::new_session(fd_server)), nullptr);
    ASSERT_NE(queue->put_sck_tls_client(fd_client, (void*) 0xC, vnet::tls::new_session(fd_client)), nullptr);
    for (int i = 0; i < 100 && connected_datas.size() < 2; i++) queue->wait_and_process();
    ASSERT_EQ(connected_datas.size(), 2u);

    // Many heartbeats in one write: one or two TLS records.
    const int many = 500;
    std::vector<uint8_t> bytes;
    PacketHeader header(0, (PacketType) 0x32);
    for (int i = 0; i < many; i++)
        bytes.insert(bytes.end(), (uint8_t*) &header, (uint8_t*) &header + sizeof(header));
    EXPECT_TRUE(queue->send(fd_client, bytes.data(), bytes.size()));

    size_t got = 0;
    for (int turns = 0; turns < 2 * many / PACKETS_PER_TURN + 4 && got < (size_t) many; turns++) {
        queue->wait_and_process();
        got = 0;
        for (const auto& d : socket_datas) {
            if (d.fd == fd_server) got++;
        }
        if (turns == 0) {
            EXPECT_LE(got, (size_t) PACKETS_PER_TURN);
        }
    }
    EXPECT_EQ(got, (size_t) many);

    delete queue;
}

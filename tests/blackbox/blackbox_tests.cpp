#include <gtest/gtest.h>
#include "vnet/blackbox/blackbox.hpp"

#include <fstream>
#include <cstring>
#include <arpa/inet.h>

using namespace vnet::blackbox;

static uint32_t ip(const char* s) {
    uint32_t addr = 0;
    inet_pton(AF_INET, s, &addr);
    return addr;
}

static std::vector<uint8_t> make_packet(const char* src, const char* dst) {
    std::vector<uint8_t> pkt(24, 0);
    pkt[0] = 0x45;
    uint16_t total = htons(24);
    std::memcpy(pkt.data() + 2, &total, 2);
    pkt[9] = PROTO_TCP;

    uint32_t sip, dip;
    inet_pton(AF_INET, src, &sip);
    inet_pton(AF_INET, dst, &dip);
    std::memcpy(pkt.data() + 12, &sip, 4);
    std::memcpy(pkt.data() + 16, &dip, 4);

    uint16_t sp = htons(1234), dp = htons(80);
    std::memcpy(pkt.data() + 20, &sp, 2);
    std::memcpy(pkt.data() + 22, &dp, 2);
    return pkt;
}

class BlackBoxTest : public ::testing::Test {
protected:
    std::string tmp_path;
    Config cfg;
    static constexpr int TUN_FD = 100;

    void write_config(const std::string& content) {
        tmp_path = "/tmp/vnet_bb_test_" + std::to_string(getpid()) + ".conf";
        std::ofstream f(tmp_path);
        f << content;
        f.close();
    }

    void SetUp() override {
        write_config(
            // IP assignments
            "contestant1  10.0.1.1\n"
            "contestant2  10.0.1.2\n"
            "contestant3  10.0.1.3\n"
            "gameserver   10.0.0.1\n"
            "\n"
            // ACL: contestants can talk to gameserver
            "contestant1  gameserver\n"
            "contestant2  gameserver\n"
            // contestant3 has no rules at all
            "\n"
            // ACL: gameserver can reach specific internet IPs
            "gameserver   ::internet:8.8.8.8\n"
            "gameserver   ::internet:1.1.1.1\n"
        );
        ASSERT_TRUE(cfg.load(tmp_path));
    }

    void TearDown() override {
        if (!tmp_path.empty()) std::remove(tmp_path.c_str());
    }
};

// ----- Agent lifecycle -----

TEST_F(BlackBoxTest, AgentRegistration) {
    BlackBox box(cfg, TUN_FD);

    EXPECT_TRUE(box.on_agent_authenticated("contestant1", 10));
    EXPECT_TRUE(box.on_agent_authenticated("gameserver", 11));
    EXPECT_EQ(box.agents().size(), 2u);
}

TEST_F(BlackBoxTest, AgentRegistrationUnknownName) {
    BlackBox box(cfg, TUN_FD);
    EXPECT_FALSE(box.on_agent_authenticated("nobody", 10));
    EXPECT_EQ(box.agents().size(), 0u);
}

TEST_F(BlackBoxTest, AgentDisconnect) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);

    box.on_agent_disconnected(10);
    EXPECT_EQ(box.agents().size(), 0u);
}

// ----- Source validation -----

TEST_F(BlackBoxTest, SpoofedSource) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);

    auto pkt = make_packet("10.0.1.2", "10.0.0.1");
    auto d = box.process(SourceType::AGENT, 10, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::SOURCE_SPOOF);
}

TEST_F(BlackBoxTest, UnknownSourceFd) {
    BlackBox box(cfg, TUN_FD);

    auto pkt = make_packet("10.0.1.1", "10.0.1.2");
    auto d = box.process(SourceType::AGENT, 99, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::SOURCE_UNKNOWN);
}

// ----- ACL: agent-to-agent -----

TEST_F(BlackBoxTest, ACLAllowedLocalDelivery) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);
    box.on_agent_authenticated("gameserver", 11);

    // contestant1 → gameserver: allowed by ACL
    auto pkt = make_packet("10.0.1.1", "10.0.0.1");
    auto d = box.process(SourceType::AGENT, 10, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DELIVER_LOCAL);
    EXPECT_EQ(d.target_fd, 11);
}

TEST_F(BlackBoxTest, ACLAllowedReverse) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);
    box.on_agent_authenticated("gameserver", 11);

    // gameserver → contestant1: allowed (bidirectional)
    auto pkt = make_packet("10.0.0.1", "10.0.1.1");
    auto d = box.process(SourceType::AGENT, 11, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DELIVER_LOCAL);
    EXPECT_EQ(d.target_fd, 10);
}

TEST_F(BlackBoxTest, ACLDeniedAgentToAgent) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);
    box.on_agent_authenticated("contestant2", 11);

    // contestant1 → contestant2: NO ACL rule between them
    auto pkt = make_packet("10.0.1.1", "10.0.1.2");
    auto d = box.process(SourceType::AGENT, 10, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::ACL_DENIED);
}

TEST_F(BlackBoxTest, ACLDeniedIsolatedAgent) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant3", 12);
    box.on_agent_authenticated("gameserver", 11);

    // contestant3 has no rules at all
    auto pkt = make_packet("10.0.1.3", "10.0.0.1");
    auto d = box.process(SourceType::AGENT, 12, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::ACL_DENIED);
}

// ----- ACL: internet -----

TEST_F(BlackBoxTest, InternetAllowedSpecificIP) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("gameserver", 11);

    // gameserver → 8.8.8.8: allowed
    auto pkt = make_packet("10.0.0.1", "8.8.8.8");
    auto d = box.process(SourceType::AGENT, 11, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::FORWARD_INTERNET);
    EXPECT_EQ(d.target_fd, TUN_FD);
}

TEST_F(BlackBoxTest, InternetDeniedWrongIP) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("gameserver", 11);

    // gameserver → 9.9.9.9: NOT in ACL
    auto pkt = make_packet("10.0.0.1", "9.9.9.9");
    auto d = box.process(SourceType::AGENT, 11, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::ACL_DENIED);
}

TEST_F(BlackBoxTest, InternetDeniedNoRules) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);

    // contestant1 has no internet rules
    auto pkt = make_packet("10.0.1.1", "8.8.8.8");
    auto d = box.process(SourceType::AGENT, 10, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::ACL_DENIED);
}

TEST_F(BlackBoxTest, InternetNoTun) {
    BlackBox box(cfg, -1);  // no TUN
    box.on_agent_authenticated("gameserver", 11);

    auto pkt = make_packet("10.0.0.1", "8.8.8.8");
    auto d = box.process(SourceType::AGENT, 11, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::DEST_UNREACHABLE);
}

// ----- Remote routing -----

TEST_F(BlackBoxTest, ForwardToSwitch) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);

    // contestant2 is known in config but not local, route exists
    box.on_route_update(ip("10.0.1.2"), 20);

    // contestant1 → contestant2: NOT allowed (no ACL between contestants)
    auto pkt = make_packet("10.0.1.1", "10.0.1.2");
    auto d = box.process(SourceType::AGENT, 10, pkt.data(), pkt.size());
    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::ACL_DENIED);
}

TEST_F(BlackBoxTest, ForwardToSwitchAllowed) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);

    // gameserver is remote, route exists
    box.on_route_update(ip("10.0.0.1"), 20);

    // contestant1 → gameserver: allowed by ACL
    auto pkt = make_packet("10.0.1.1", "10.0.0.1");
    auto d = box.process(SourceType::AGENT, 10, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::FORWARD_SWITCH);
    EXPECT_EQ(d.target_fd, 20);
}

TEST_F(BlackBoxTest, RequestRouteWhenUnknown) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);

    // gameserver is remote, no route set, but ACL allows
    auto pkt = make_packet("10.0.1.1", "10.0.0.1");
    auto d = box.process(SourceType::AGENT, 10, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::REQUEST_ROUTE);
    EXPECT_EQ(d.dest_ipv4, ip("10.0.0.1"));
}

// ----- Packets from peer switch -----

TEST_F(BlackBoxTest, SwitchDeliverLocal) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);

    auto pkt = make_packet("10.0.0.1", "10.0.1.1");
    auto d = box.process(SourceType::SWITCH, 20, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DELIVER_LOCAL);
    EXPECT_EQ(d.target_fd, 10);
}

TEST_F(BlackBoxTest, SwitchDestNotLocal) {
    BlackBox box(cfg, TUN_FD);

    auto pkt = make_packet("10.0.0.1", "10.0.1.1");
    auto d = box.process(SourceType::SWITCH, 20, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::DEST_UNREACHABLE);
}

// ----- Packets from TUN (internet return traffic) -----

TEST_F(BlackBoxTest, TunReturnTrafficAllowed) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("gameserver", 11);

    // Return traffic from 8.8.8.8 → gameserver: allowed
    auto pkt = make_packet("8.8.8.8", "10.0.0.1");
    auto d = box.process(SourceType::TUN, TUN_FD, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DELIVER_LOCAL);
    EXPECT_EQ(d.target_fd, 11);
}

TEST_F(BlackBoxTest, TunReturnTrafficDenied) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("gameserver", 11);

    // Return traffic from 9.9.9.9: gameserver has no rule for this IP
    auto pkt = make_packet("9.9.9.9", "10.0.0.1");
    auto d = box.process(SourceType::TUN, TUN_FD, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::ACL_DENIED);
}

TEST_F(BlackBoxTest, TunDestNotLocal) {
    BlackBox box(cfg, TUN_FD);

    auto pkt = make_packet("8.8.8.8", "10.0.0.1");
    auto d = box.process(SourceType::TUN, TUN_FD, pkt.data(), pkt.size());

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::DEST_UNREACHABLE);
}

// ----- Malformed packets -----

TEST_F(BlackBoxTest, MalformedPacket) {
    BlackBox box(cfg, TUN_FD);
    box.on_agent_authenticated("contestant1", 10);

    uint8_t garbage[5] = {0xFF, 0, 0, 0, 0};
    auto d = box.process(SourceType::AGENT, 10, garbage, sizeof(garbage));

    EXPECT_EQ(d.action, PacketAction::DROP);
    EXPECT_EQ(d.drop_reason, DropReason::MALFORMED_PACKET);
}

// ----- Route cleanup -----

TEST_F(BlackBoxTest, SwitchDisconnectClearsRoutes) {
    BlackBox box(cfg, TUN_FD);
    box.on_route_update(ip("10.0.1.2"), 20);
    box.on_route_update(ip("10.0.0.1"), 20);

    box.on_switch_disconnected(20);
    EXPECT_EQ(box.routes().size(), 0u);
}

TEST_F(BlackBoxTest, RouteUpdateRemove) {
    BlackBox box(cfg, TUN_FD);
    box.on_route_update(ip("10.0.1.2"), 20);
    EXPECT_EQ(box.routes().size(), 1u);

    box.on_route_update(ip("10.0.1.2"), -1);
    EXPECT_EQ(box.routes().size(), 0u);
}

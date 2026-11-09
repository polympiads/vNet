#include <gtest/gtest.h>
#include "vnet/blackbox/config.hpp"

#include <fstream>
#include <arpa/inet.h>

using namespace vnet::blackbox;

static uint32_t ip(const char* s) {
    uint32_t addr = 0;
    inet_pton(AF_INET, s, &addr);
    return addr;
}

class ConfigTest : public ::testing::Test {
protected:
    std::string tmp_path;

    void write_config(const std::string& content) {
        tmp_path = "/tmp/vnet_test_config_" + std::to_string(getpid()) + ".conf";
        std::ofstream f(tmp_path);
        f << content;
        f.close();
    }

    void TearDown() override {
        if (!tmp_path.empty()) {
            std::remove(tmp_path.c_str());
        }
    }
};

TEST_F(ConfigTest, IPAssignments) {
    write_config(
        "agent1  10.0.1.1\n"
        "agent2  10.0.1.2\n"
    );

    Config cfg;
    ASSERT_TRUE(cfg.load(tmp_path));

    EXPECT_EQ(cfg.find_agent_for_ip(ip("10.0.1.1")), "agent1");
    EXPECT_EQ(cfg.find_agent_for_ip(ip("10.0.1.2")), "agent2");
    EXPECT_EQ(cfg.find_ip_for_agent("agent1"), ip("10.0.1.1"));
    EXPECT_EQ(cfg.agents().size(), 2u);
}

TEST_F(ConfigTest, AgentToAgentACL) {
    write_config(
        "agent1  10.0.1.1\n"
        "agent2  10.0.1.2\n"
        "agent3  10.0.1.3\n"
        "\n"
        "agent1  agent2\n"
    );

    Config cfg;
    ASSERT_TRUE(cfg.load(tmp_path));

    // Bidirectional
    EXPECT_TRUE(cfg.can_agents_communicate(ip("10.0.1.1"), ip("10.0.1.2")));
    EXPECT_TRUE(cfg.can_agents_communicate(ip("10.0.1.2"), ip("10.0.1.1")));

    // No rule between agent1 and agent3
    EXPECT_FALSE(cfg.can_agents_communicate(ip("10.0.1.1"), ip("10.0.1.3")));
    EXPECT_FALSE(cfg.can_agents_communicate(ip("10.0.1.3"), ip("10.0.1.1")));

    // No rule between agent2 and agent3
    EXPECT_FALSE(cfg.can_agents_communicate(ip("10.0.1.2"), ip("10.0.1.3")));
}

TEST_F(ConfigTest, InternetACL) {
    write_config(
        "server  10.0.0.1\n"
        "client  10.0.1.1\n"
        "\n"
        "server  ::internet:8.8.8.8\n"
        "server  ::internet:1.1.1.1\n"
    );

    Config cfg;
    ASSERT_TRUE(cfg.load(tmp_path));

    EXPECT_TRUE(cfg.can_reach_internet_ip(ip("10.0.0.1"), ip("8.8.8.8")));
    EXPECT_TRUE(cfg.can_reach_internet_ip(ip("10.0.0.1"), ip("1.1.1.1")));
    EXPECT_FALSE(cfg.can_reach_internet_ip(ip("10.0.0.1"), ip("9.9.9.9")));

    // Client has no internet rules
    EXPECT_FALSE(cfg.can_reach_internet_ip(ip("10.0.1.1"), ip("8.8.8.8")));
    EXPECT_FALSE(cfg.has_any_internet_access(ip("10.0.1.1")));
    EXPECT_TRUE(cfg.has_any_internet_access(ip("10.0.0.1")));
}

TEST_F(ConfigTest, CommentsAndBlankLines) {
    write_config(
        "# This is a comment\n"
        "\n"
        "   # Indented comment\n"
        "agent1  10.0.1.1\n"
        "\n"
    );

    Config cfg;
    ASSERT_TRUE(cfg.load(tmp_path));
    EXPECT_EQ(cfg.agents().size(), 1u);
}

TEST_F(ConfigTest, UnknownAgent) {
    write_config("agent1  10.0.1.1\n");

    Config cfg;
    ASSERT_TRUE(cfg.load(tmp_path));
    EXPECT_EQ(cfg.find_agent_for_ip(ip("10.0.1.2")), "");
    EXPECT_EQ(cfg.find_ip_for_agent("nope"), 0u);
}

TEST_F(ConfigTest, InvalidIPFails) {
    write_config("agent1  not_an_ip\n");

    Config cfg;
    EXPECT_FALSE(cfg.load(tmp_path));
}

TEST_F(ConfigTest, DuplicateIPFails) {
    write_config(
        "agent1  10.0.1.1\n"
        "agent2  10.0.1.1\n"
    );

    Config cfg;
    EXPECT_FALSE(cfg.load(tmp_path));
}

TEST_F(ConfigTest, DuplicateNameFails) {
    write_config(
        "agent1  10.0.1.1\n"
        "agent1  10.0.1.2\n"
    );

    Config cfg;
    EXPECT_FALSE(cfg.load(tmp_path));
}

TEST_F(ConfigTest, ACLUnknownSourceFails) {
    write_config(
        "agent1  10.0.1.1\n"
        "ghost   agent1\n"
    );

    Config cfg;
    EXPECT_FALSE(cfg.load(tmp_path));
}

TEST_F(ConfigTest, ACLUnknownDestFails) {
    write_config(
        "agent1  10.0.1.1\n"
        "agent1  ghost\n"
    );

    Config cfg;
    EXPECT_FALSE(cfg.load(tmp_path));
}

TEST_F(ConfigTest, InternetBadIPFails) {
    write_config(
        "agent1  10.0.1.1\n"
        "agent1  ::internet:not_an_ip\n"
    );

    Config cfg;
    EXPECT_FALSE(cfg.load(tmp_path));
}

TEST_F(ConfigTest, MissingFileFails) {
    Config cfg;
    EXPECT_FALSE(cfg.load("/tmp/this_file_does_not_exist_12345.conf"));
}

TEST_F(ConfigTest, MalformedLineFails) {
    write_config("agent1\n");

    Config cfg;
    EXPECT_FALSE(cfg.load(tmp_path));
}

TEST_F(ConfigTest, InternetCidr) {
    write_config(
        "server  10.0.0.1\n"
        "server  ::internet:192.0.2.0/24\n"
        "server  ::internet:198.51.100.7\n"
        "server  ::internet:0.0.0.0/0\n"
        "client  10.0.1.1\n"
        "client  ::internet:203.0.113.0/25\n"
    );
    Config cfg;
    ASSERT_TRUE(cfg.load(tmp_path));

    EXPECT_TRUE(cfg.can_reach_internet_ip(ip("10.0.0.1"), ip("192.0.2.77")));
    EXPECT_TRUE(cfg.can_reach_internet_ip(ip("10.0.0.1"), ip("198.51.100.7")));
    EXPECT_TRUE(cfg.can_reach_internet_ip(ip("10.0.0.1"), ip("8.8.8.8")));   // /0

    EXPECT_TRUE(cfg.can_reach_internet_ip(ip("10.0.1.1"), ip("203.0.113.1")));
    EXPECT_TRUE(cfg.can_reach_internet_ip(ip("10.0.1.1"), ip("203.0.113.127")));
    EXPECT_FALSE(cfg.can_reach_internet_ip(ip("10.0.1.1"), ip("203.0.113.128")));
    EXPECT_FALSE(cfg.can_reach_internet_ip(ip("10.0.1.1"), ip("192.0.2.77")));
}

TEST_F(ConfigTest, InternetBadCidrFails) {
    write_config("agent1  10.0.0.1\nagent1  ::internet:192.0.2.0/33\n");
    Config cfg;
    EXPECT_FALSE(cfg.load(tmp_path));
    write_config("agent1  10.0.0.1\nagent1  ::internet:0.0.0.0\n");
    EXPECT_FALSE(cfg.load(tmp_path));
}

TEST_F(ConfigTest, BadAgentNameFails) {
    write_config("agent-is-too-long  10.0.0.1\n");
    Config cfg;
    EXPECT_FALSE(cfg.load(tmp_path));
    write_config("agent;1  10.0.0.1\n");
    EXPECT_FALSE(cfg.load(tmp_path));
    write_config("agent_1  10.0.0.1\n");
    EXPECT_TRUE(cfg.load(tmp_path));
}

TEST_F(ConfigTest, FailedReloadKeepsOldConfig) {
    write_config("agent1  10.0.0.1\nagent2  10.0.0.2\nagent1  agent2\n");
    Config cfg;
    ASSERT_TRUE(cfg.load(tmp_path));
    std::string before = cfg.fingerprint();

    write_config("agent1  10.0.0.1\nagent1  nobody\n");
    EXPECT_FALSE(cfg.load(tmp_path));
    EXPECT_EQ(cfg.find_ip_for_agent("agent2"), ip("10.0.0.2"));
    EXPECT_TRUE(cfg.can_agents_communicate(ip("10.0.0.1"), ip("10.0.0.2")));
    EXPECT_EQ(cfg.fingerprint(), before);
}

TEST_F(ConfigTest, FingerprintIgnoresOrderAndSpacing) {
    write_config(
        "agent1  10.0.0.1\n"
        "agent2  10.0.0.2\n"
        "agent1  agent2\n"
        "agent1  ::internet:8.8.8.8\n"
    );
    Config a;
    ASSERT_TRUE(a.load(tmp_path));

    write_config(
        "# same thing, other order\n"
        "agent2 10.0.0.2\n"
        "\tagent1\t::internet:8.8.8.8/32\n"
        "agent2 agent1\n"
        "agent1 10.0.0.1\n"
    );
    Config b;
    ASSERT_TRUE(b.load(tmp_path));
    EXPECT_EQ(a.fingerprint(), b.fingerprint());
    EXPECT_EQ(a.fingerprint().size(), 64u);

    write_config("agent1  10.0.0.1\nagent2  10.0.0.2\n");
    Config c;
    ASSERT_TRUE(c.load(tmp_path));
    EXPECT_NE(a.fingerprint(), c.fingerprint());
}

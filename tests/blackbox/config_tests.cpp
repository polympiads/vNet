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

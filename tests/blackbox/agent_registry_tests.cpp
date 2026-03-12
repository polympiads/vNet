#include <gtest/gtest.h>
#include "vnet/blackbox/agent_registry.hpp"

#include <arpa/inet.h>

using namespace vnet::blackbox;

static uint32_t ip(const char* s) {
    uint32_t addr = 0;
    inet_pton(AF_INET, s, &addr);
    return addr;
}

TEST(AgentRegistry, RegisterAndLookup) {
    AgentRegistry reg;

    auto* e = reg.register_agent("agent1", ip("10.0.1.1"), 5);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->name, "agent1");
    EXPECT_EQ(e->virtual_ipv4, ip("10.0.1.1"));
    EXPECT_EQ(e->fd, 5);

    EXPECT_EQ(reg.find_by_ipv4(ip("10.0.1.1")), e);
    EXPECT_EQ(reg.find_by_fd(5), e);
    EXPECT_EQ(reg.find_by_name("agent1"), e);
    EXPECT_EQ(reg.size(), 1u);
}

TEST(AgentRegistry, LookupMiss) {
    AgentRegistry reg;
    reg.register_agent("agent1", ip("10.0.1.1"), 5);

    EXPECT_EQ(reg.find_by_ipv4(ip("10.0.1.2")), nullptr);
    EXPECT_EQ(reg.find_by_fd(99), nullptr);
    EXPECT_EQ(reg.find_by_name("nope"), nullptr);
}

TEST(AgentRegistry, UnregisterByFd) {
    AgentRegistry reg;
    reg.register_agent("agent1", ip("10.0.1.1"), 5);
    reg.register_agent("agent2", ip("10.0.1.2"), 6);

    reg.unregister_by_fd(5);

    EXPECT_EQ(reg.find_by_fd(5), nullptr);
    EXPECT_EQ(reg.find_by_ipv4(ip("10.0.1.1")), nullptr);
    EXPECT_EQ(reg.find_by_name("agent1"), nullptr);
    EXPECT_EQ(reg.size(), 1u);

    EXPECT_NE(reg.find_by_fd(6), nullptr);
}

TEST(AgentRegistry, UnregisterByName) {
    AgentRegistry reg;
    reg.register_agent("agent1", ip("10.0.1.1"), 5);

    reg.unregister_by_name("agent1");
    EXPECT_EQ(reg.size(), 0u);
    EXPECT_EQ(reg.find_by_fd(5), nullptr);
}

TEST(AgentRegistry, ReregisterSameName) {
    AgentRegistry reg;
    reg.register_agent("agent1", ip("10.0.1.1"), 5);
    auto* e2 = reg.register_agent("agent1", ip("10.0.1.1"), 7);

    EXPECT_EQ(reg.size(), 1u);
    EXPECT_EQ(reg.find_by_fd(5), nullptr);
    EXPECT_EQ(reg.find_by_fd(7), e2);
}

TEST(AgentRegistry, MultipleAgents) {
    AgentRegistry reg;
    reg.register_agent("a1", ip("10.0.1.1"), 10);
    reg.register_agent("a2", ip("10.0.1.2"), 11);
    reg.register_agent("a3", ip("10.0.1.3"), 12);

    EXPECT_EQ(reg.size(), 3u);
    EXPECT_EQ(reg.find_by_name("a2")->fd, 11);
}

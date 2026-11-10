#include <gtest/gtest.h>
#include "common/bypass.h"

#include <arpa/inet.h>
#include <fstream>
#include <unistd.h>

static uint32_t ip(const char* s) {
    uint32_t addr = 0;
    inet_pton(AF_INET, s, &addr);
    return addr;
}

static const char* OVERLAY = "10.0.0.0/8";

TEST(Bypass, ParsesEveryKind) {
    std::vector<BypassRule> rules;
    ASSERT_TRUE(parse_bypass(
        "# the broker and its friends\r\n"
        "route 172.16.5.0/24\n"
        "\n"
        "  route\t192.0.2.7\n"
        "out -p tcp -d 172.16.5.10 --dport 4001:4002 -j ACCEPT\n"
        "in  -p tcp -s 172.20.0.0/16 --dport 22443 -j ACCEPT\n"
        "in6 -p tcp --dport 22 -j ACCEPT\n"
        "out6 -p udp --dport 53 -j ACCEPT\n",
        OVERLAY, rules, "[Test]"));
    ASSERT_EQ(rules.size(), 6u);

    EXPECT_EQ(rules[0].kind, BypassRule::ROUTE);
    EXPECT_EQ(rules[0].cidr, "172.16.5.0/24");
    EXPECT_EQ(rules[0].addr, ip("172.16.5.0"));
    EXPECT_EQ(rules[0].prefix, 24);

    // A bare address is a /32.
    EXPECT_EQ(rules[1].kind, BypassRule::ROUTE);
    EXPECT_EQ(rules[1].cidr, "192.0.2.7/32");
    EXPECT_EQ(rules[1].prefix, 32);

    EXPECT_EQ(rules[2].kind, BypassRule::OUT);
    ASSERT_EQ(rules[2].args.size(), 8u);
    EXPECT_EQ(rules[2].args[0], "-p");
    EXPECT_EQ(rules[2].args[4], "--dport");
    EXPECT_EQ(rules[2].args[7], "ACCEPT");

    EXPECT_EQ(rules[3].kind, BypassRule::IN);
    EXPECT_EQ(rules[3].args.size(), 8u);
    EXPECT_EQ(rules[4].kind, BypassRule::IN6);
    EXPECT_EQ(rules[5].kind, BypassRule::OUT6);
}

TEST(Bypass, HostBitsAreCleared) {
    std::vector<BypassRule> rules;
    ASSERT_TRUE(parse_bypass("route 172.16.5.77/24\n", OVERLAY, rules, "[Test]"));
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].cidr, "172.16.5.0/24");
    EXPECT_EQ(rules[0].addr, ip("172.16.5.0"));
}

TEST(Bypass, EmptyFileIsNoRule) {
    std::vector<BypassRule> rules;
    rules.push_back(BypassRule{});
    ASSERT_TRUE(parse_bypass("# nothing\n\n", OVERLAY, rules, "[Test]"));
    EXPECT_TRUE(rules.empty());
}

TEST(Bypass, BadLinesAreRefused) {
    std::vector<BypassRule> rules;
    rules.push_back(BypassRule{});
    const char* bad[] = {
        "route\n",                          // no prefix
        "route 172.16.5.0/24 extra\n",      // too many words
        "route 172.16.5.0/33\n",            // bad prefix length
        "route not-an-address\n",           // bad address
        "route 0.0.0.0/0\n",                // the default route
        "route 10.3.0.0/16\n",              // inside the overlay
        "route 0.0.0.0/4\n",                // contains the overlay
        "in\n",                             // no arguments
        "allow 172.16.5.0/24\n",            // unknown kind
        "route 172.16.5.0/24\nnonsense\n",  // a good line, then a bad one
    };
    for (const char* text : bad) {
        EXPECT_FALSE(parse_bypass(text, OVERLAY, rules, "[Test]")) << text;
        // The caller's rules are untouched when the file is refused.
        EXPECT_EQ(rules.size(), 1u) << text;
    }
}

TEST(Bypass, OverlayPrefixMustParse) {
    std::vector<BypassRule> rules;
    EXPECT_FALSE(parse_bypass("route 172.16.5.0/24\n", "overlay", rules, "[Test]"));
}

TEST(Bypass, LoadsFromFile) {
    std::string path = "/tmp/vnet_bypass_" + std::to_string(getpid()) + ".conf";
    {
        std::ofstream f(path);
        f << "route 172.16.5.0/24\nin -p tcp --dport 22443 -j ACCEPT\n";
    }
    std::vector<BypassRule> rules;
    EXPECT_TRUE(load_bypass_file(path, OVERLAY, rules, "[Test]"));
    EXPECT_EQ(rules.size(), 2u);
    unlink(path.c_str());

    // A path that does not exist is an error, not an empty file.
    EXPECT_FALSE(load_bypass_file(path, OVERLAY, rules, "[Test]"));
}

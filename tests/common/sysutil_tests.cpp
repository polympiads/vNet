#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <arpa/inet.h>

#include "common/sysutil.h"

TEST(SysUtil, ValidName) {
    EXPECT_TRUE(valid_name("agent1", AGENT_NAME_MAX));
    EXPECT_TRUE(valid_name("a-b_C9", AGENT_NAME_MAX));
    EXPECT_TRUE(valid_name("0123456789", AGENT_NAME_MAX));
    EXPECT_FALSE(valid_name("", AGENT_NAME_MAX));
    EXPECT_FALSE(valid_name("01234567890", AGENT_NAME_MAX));
    EXPECT_FALSE(valid_name("agent 1", AGENT_NAME_MAX));
    EXPECT_FALSE(valid_name("a;rm", AGENT_NAME_MAX));
    EXPECT_FALSE(valid_name("a/b", AGENT_NAME_MAX));
    EXPECT_FALSE(valid_name("a$(x)", AGENT_NAME_MAX));
    EXPECT_TRUE(valid_name("01234567890", SWITCH_NAME_MAX));
}

TEST(SysUtil, ParseCidr) {
    uint32_t addr = 0;
    int prefix = -1;
    EXPECT_TRUE(parse_cidr("10.0.0.0/8", addr, prefix));
    EXPECT_EQ(prefix, 8);
    uint32_t expect = 0;
    inet_pton(AF_INET, "10.0.0.0", &expect);
    EXPECT_EQ(addr, expect);

    EXPECT_TRUE(parse_cidr("8.8.8.8", addr, prefix));
    EXPECT_EQ(prefix, 32);
    EXPECT_TRUE(parse_cidr("0.0.0.0/0", addr, prefix));
    EXPECT_EQ(prefix, 0);

    EXPECT_FALSE(parse_cidr("10.0.0.0/33", addr, prefix));
    EXPECT_FALSE(parse_cidr("10.0.0.0/", addr, prefix));
    EXPECT_FALSE(parse_cidr("10.0.0.0/-1", addr, prefix));
    EXPECT_FALSE(parse_cidr("10.0.0/8", addr, prefix));
    EXPECT_FALSE(parse_cidr("10.0.0.0/8 dev", addr, prefix));
    EXPECT_FALSE(parse_cidr("", addr, prefix));
}

TEST(SysUtil, EnvInt) {
    long value = 7;
    unsetenv("VNET_TEST_INT");
    EXPECT_TRUE(env_int("VNET_TEST_INT", 0, 100, value));
    EXPECT_EQ(value, 7);

    setenv("VNET_TEST_INT", "42", 1);
    EXPECT_TRUE(env_int("VNET_TEST_INT", 0, 100, value));
    EXPECT_EQ(value, 42);

    setenv("VNET_TEST_INT", "15s", 1);
    EXPECT_FALSE(env_int("VNET_TEST_INT", 0, 100, value));
    EXPECT_EQ(value, 42);

    setenv("VNET_TEST_INT", "101", 1);
    EXPECT_FALSE(env_int("VNET_TEST_INT", 0, 100, value));

    setenv("VNET_TEST_INT", "-1", 1);
    EXPECT_FALSE(env_int("VNET_TEST_INT", 0, 100, value));

    setenv("VNET_TEST_INT", "", 1);
    EXPECT_TRUE(env_int("VNET_TEST_INT", 0, 100, value));
    EXPECT_EQ(value, 42);
    unsetenv("VNET_TEST_INT");
}

TEST(SysUtil, EnvOr) {
    unsetenv("VNET_TEST_STR");
    EXPECT_EQ(env_or("VNET_TEST_STR", "def"), "def");
    setenv("VNET_TEST_STR", "x", 1);
    EXPECT_EQ(env_or("VNET_TEST_STR", "def"), "x");
    unsetenv("VNET_TEST_STR");
}

TEST(SysUtil, RunProgram) {
    EXPECT_TRUE(run_program({"true"}, "[Test]"));
    EXPECT_FALSE(run_program({"false"}, "[Test]", true));
    EXPECT_FALSE(run_program({"/nonexistent/program"}, "[Test]", true));
    EXPECT_FALSE(run_program({}, "[Test]"));
    // Arguments reach the program as given, with no shell in between.
    EXPECT_FALSE(run_program({"test", "a;b", "=", "a"}, "[Test]", true));
    EXPECT_TRUE(run_program({"test", "a;b", "=", "a;b"}, "[Test]", true));
}

TEST(SysUtil, LoadAuthKey) {
    unsetenv("VNET_AUTH_KEY");
    unsetenv("VNET_AUTH_KEY_FILE");
    EXPECT_EQ(load_auth_key(nullptr, "[Test]"), "");
    EXPECT_EQ(load_auth_key("argkey", "[Test]"), "argkey");

    std::string path = "/tmp/vnet_key_test_" + std::to_string(getpid());
    {
        std::ofstream out(path);
        out << "filekey \n" << "second line\n";
    }
    setenv("VNET_AUTH_KEY_FILE", path.c_str(), 1);
    EXPECT_EQ(load_auth_key("argkey", "[Test]"), "filekey");

    setenv("VNET_AUTH_KEY", "envkey", 1);
    EXPECT_EQ(load_auth_key("argkey", "[Test]"), "envkey");

    unsetenv("VNET_AUTH_KEY");
    setenv("VNET_AUTH_KEY_FILE", "/nonexistent/key", 1);
    EXPECT_EQ(load_auth_key("argkey", "[Test]"), "");

    unsetenv("VNET_AUTH_KEY_FILE");
    unlink(path.c_str());
}

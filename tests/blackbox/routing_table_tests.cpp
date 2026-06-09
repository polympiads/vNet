#include <gtest/gtest.h>
#include "vnet/blackbox/routing_table.hpp"

#include <arpa/inet.h>

using namespace vnet::blackbox;

static uint32_t ip(const char* s) {
    uint32_t addr = 0;
    inet_pton(AF_INET, s, &addr);
    return addr;
}

TEST(RoutingTable, SetAndLookup) {
    RoutingTable rt;
    rt.set_route(ip("10.0.1.1"), 7);

    EXPECT_EQ(rt.lookup(ip("10.0.1.1")), 7);
    EXPECT_EQ(rt.size(), 1u);
}

TEST(RoutingTable, LookupMiss) {
    RoutingTable rt;
    EXPECT_EQ(rt.lookup(ip("10.0.1.1")), -1);
}

TEST(RoutingTable, UpdateRoute) {
    RoutingTable rt;
    rt.set_route(ip("10.0.1.1"), 7);
    rt.set_route(ip("10.0.1.1"), 8);

    EXPECT_EQ(rt.lookup(ip("10.0.1.1")), 8);
    EXPECT_EQ(rt.size(), 1u);
}

TEST(RoutingTable, RemoveRoute) {
    RoutingTable rt;
    rt.set_route(ip("10.0.1.1"), 7);
    rt.remove_route(ip("10.0.1.1"));

    EXPECT_EQ(rt.lookup(ip("10.0.1.1")), -1);
    EXPECT_EQ(rt.size(), 0u);
}

TEST(RoutingTable, RemoveRoutesForSwitch) {
    RoutingTable rt;
    rt.set_route(ip("10.0.1.1"), 7);
    rt.set_route(ip("10.0.1.2"), 7);
    rt.set_route(ip("10.0.1.3"), 8);

    rt.remove_routes_for_switch(7);

    EXPECT_EQ(rt.lookup(ip("10.0.1.1")), -1);
    EXPECT_EQ(rt.lookup(ip("10.0.1.2")), -1);
    EXPECT_EQ(rt.lookup(ip("10.0.1.3")), 8);
    EXPECT_EQ(rt.size(), 1u);
}

TEST(RoutingTable, Clear) {
    RoutingTable rt;
    rt.set_route(ip("10.0.1.1"), 7);
    rt.set_route(ip("10.0.1.2"), 8);
    rt.clear();

    EXPECT_EQ(rt.size(), 0u);
}

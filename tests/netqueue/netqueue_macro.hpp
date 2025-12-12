
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <iostream>
#include <utility>

#include "vnet/netqueue/netqueue.hpp"
#include "vnet/protocol/header.hpp"
#include "utils/malloc_wrapper.hpp"
#include "utils/tun_wrapper.hpp"

#define PREPARE_SOCKET_PAIR \
    int sv[2]; \
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0); \
    int fd_client = sv[0]; \
    int fd_server = sv[1];

#define PREPARE_NETWORK_TEST(state) \
    PREPARE_SOCKET_PAIR \
    NetworkElement* element = new NetworkElement(fd_client, nullptr, state); \
    EXPECT_NE(element, nullptr);

#define PREPARE_TUN_PAIR \
    int sv[2]; \
    tun_wrapper_open(sv); \
    int fd_client = sv[0]; \
    int fd_server = sv[1];

#define PREPARE_NETWORK_TUN_TEST(state) \
    PREPARE_TUN_PAIR \
    NetworkElement* element = new NetworkElement(fd_client, nullptr, state); \
    EXPECT_NE(element, nullptr);

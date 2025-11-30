
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <iostream>
#include <utility>

#include "utils/tun_wrapper.hpp"
#include "vnet/const.hpp"

TEST(TunWrapper, ReadWriteWorks) {
    int sv[2];
    tun_wrapper_open(sv);
    int fd_client = sv[0];
    int fd_server = sv[1];

    unsigned char buffer[vnet::NET_BUFFER_SIZE];
    write(fd_server, "sample1", 7);
    write(fd_server, "sample23", 8);
    write(fd_server, "sample45", 6);

    ssize_t nb_read;
    nb_read = read(fd_client, buffer, vnet::NET_BUFFER_SIZE);
    EXPECT_EQ(nb_read, 7);
    EXPECT_EQ(buffer[0], 's');
    EXPECT_EQ(buffer[1], 'a');
    EXPECT_EQ(buffer[2], 'm');
    EXPECT_EQ(buffer[3], 'p');
    EXPECT_EQ(buffer[4], 'l');
    EXPECT_EQ(buffer[5], 'e');
    EXPECT_EQ(buffer[6], '1');
    nb_read = read(fd_client, buffer, vnet::NET_BUFFER_SIZE);
    EXPECT_EQ(nb_read, 8);
    EXPECT_EQ(buffer[0], 's');
    EXPECT_EQ(buffer[1], 'a');
    EXPECT_EQ(buffer[2], 'm');
    EXPECT_EQ(buffer[3], 'p');
    EXPECT_EQ(buffer[4], 'l');
    EXPECT_EQ(buffer[5], 'e');
    EXPECT_EQ(buffer[6], '2');
    EXPECT_EQ(buffer[7], '3');
    nb_read = read(fd_client, buffer, 6);
    EXPECT_EQ(nb_read, 6);
    EXPECT_EQ(buffer[0], 's');
    EXPECT_EQ(buffer[1], 'a');
    EXPECT_EQ(buffer[2], 'm');
    EXPECT_EQ(buffer[3], 'p');
    EXPECT_EQ(buffer[4], 'l');
    EXPECT_EQ(buffer[5], 'e');

    tun_wrapper_close(fd_client);
    tun_wrapper_close(fd_server);
}

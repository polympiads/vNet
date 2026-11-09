#include "netqueue/netqueue_macro.hpp"
#include "mip.pb.h"

#include <cstring>

using namespace vnet::netqueue;
using namespace vnet::protocol;

/* Read everything waiting on fd into out; stops at EAGAIN. */
static std::vector<uint8_t> drain(int fd) {
    std::vector<uint8_t> out;
    uint8_t chunk[65536];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        out.insert(out.end(), chunk, chunk + n);
    }
    return out;
}

TEST(NetQueueSend, ProtobufFrameArrivesWhole) {
    PREPARE_SOCKET_PAIR
    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    ASSERT_NE(queue->put_sck(fd_client, nullptr), nullptr);

    vnet::protocol::mip::PacketSwitchHello hello;
    hello.set_switch_name("switch1");
    EXPECT_TRUE(queue->send(fd_client, PacketType::SWITCH_HELLO, hello));

    std::vector<uint8_t> got = drain(fd_server);
    ASSERT_EQ(got.size(), PACKET_HEADER_SIZE + hello.ByteSizeLong());

    PacketHeader hdr;
    memcpy(&hdr, got.data(), PACKET_HEADER_SIZE);
    EXPECT_EQ(hdr.get_packet_type(), PacketType::SWITCH_HELLO);
    EXPECT_EQ(hdr.get_payload_size(), hello.ByteSizeLong());

    vnet::protocol::mip::PacketSwitchHello parsed;
    ASSERT_TRUE(parsed.ParseFromArray(got.data() + PACKET_HEADER_SIZE,
                                      static_cast<int>(hello.ByteSizeLong())));
    EXPECT_EQ(parsed.switch_name(), "switch1");

    delete queue;
    close(fd_server);
}

TEST(NetQueueSend, DataFrameIsHeaderPlusBytes) {
    PREPARE_SOCKET_PAIR
    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    ASSERT_NE(queue->put_sck(fd_client, nullptr), nullptr);

    uint8_t datagram[40];
    for (size_t i = 0; i < sizeof(datagram); i++) datagram[i] = static_cast<uint8_t>(i * 7);
    EXPECT_TRUE(queue->send_data(fd_client, PacketType::IPV4_RAW, datagram, sizeof(datagram)));

    std::vector<uint8_t> got = drain(fd_server);
    ASSERT_EQ(got.size(), PACKET_HEADER_SIZE + sizeof(datagram));

    PacketHeader hdr;
    memcpy(&hdr, got.data(), PACKET_HEADER_SIZE);
    EXPECT_EQ(hdr.get_packet_type(), PacketType::IPV4_RAW);
    EXPECT_EQ(hdr.get_payload_size(), sizeof(datagram));
    EXPECT_EQ(memcmp(got.data() + PACKET_HEADER_SIZE, datagram, sizeof(datagram)), 0);

    delete queue;
    close(fd_server);
}

TEST(NetQueueSend, DataTooLargeIsRefused) {
    PREPARE_SOCKET_PAIR
    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    NetworkElement* element = queue->put_sck(fd_client, nullptr);
    ASSERT_NE(element, nullptr);

    std::vector<uint8_t> big(MAX_PAYLOAD_SIZE + 1, 0);
    EXPECT_FALSE(queue->send_data(fd_client, PacketType::IPV4_RAW, big.data(), big.size()));
    EXPECT_NE(element->state, SCK_ERROR);
    EXPECT_TRUE(drain(fd_server).empty());

    delete queue;
    close(fd_server);
}

/*
 * A peer that stops reading: data packets are dropped once the write
 * buffer is full, the link stays up, and a control packet that still
 * cannot fit closes it.
 */
TEST(NetQueueSend, FullBufferDropsDataAndKeepsLink) {
    PREPARE_SOCKET_PAIR
    int small = 4096;
    setsockopt(fd_client, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(fd_server, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    NetworkElement* element = queue->put_sck(fd_client, nullptr);
    ASSERT_NE(element, nullptr);

    std::vector<uint8_t> datagram(MAX_PAYLOAD_SIZE, 0xAB);
    size_t accepted = 0, dropped = 0;
    for (int i = 0; i < 200; i++) {
        if (queue->send_data(fd_client, PacketType::IPV4_RAW, datagram.data(), datagram.size()))
            accepted++;
        else
            dropped++;
    }
    EXPECT_GT(accepted, 0u);
    EXPECT_GT(dropped, 0u);
    EXPECT_NE(element->state, SCK_ERROR);
    EXPECT_LE(element->write_buffer.size() - element->write_buffer_offset, WRITE_BUFFER_MAX);

    // The queue did not take more than the limit plus what the kernel holds.
    EXPECT_LE(accepted * (PACKET_HEADER_SIZE + datagram.size()),
              WRITE_BUFFER_MAX + PACKET_HEADER_SIZE + datagram.size() + 1024 * 1024);

    // A control packet that does not fit closes the link.
    std::vector<uint8_t> control(WRITE_BUFFER_MAX, 0);
    EXPECT_FALSE(queue->send(fd_client, control.data(), control.size()));
    EXPECT_EQ(element->state, SCK_ERROR);

    delete queue;
    close(fd_server);
}

/*
 * Once the peer reads again, the buffered bytes are flushed in order.
 */
TEST(NetQueueSend, BufferedBytesFlushInOrder) {
    PREPARE_SOCKET_PAIR
    int small = 4096;
    setsockopt(fd_client, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(fd_server, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));

    NetQueue* queue = new NetQueue(NetworkQueueHandler(), 50);
    NetworkElement* element = queue->put_sck(fd_client, nullptr);
    ASSERT_NE(element, nullptr);

    // Enough sequential bytes to overflow the kernel buffer several times.
    std::vector<uint8_t> all;
    for (int i = 0; i < 40; i++) {
        std::vector<uint8_t> chunk(8000);
        for (size_t k = 0; k < chunk.size(); k++)
            chunk[k] = static_cast<uint8_t>((i * chunk.size() + k) % 251);
        if (!queue->send_data(fd_client, PacketType::IPV4_RAW, chunk.data(), chunk.size()))
            break;
        PacketHeader hdr(static_cast<uint32_t>(chunk.size()), PacketType::IPV4_RAW);
        const uint8_t* h = reinterpret_cast<const uint8_t*>(&hdr);
        all.insert(all.end(), h, h + PACKET_HEADER_SIZE);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    EXPECT_FALSE(element->write_buffer.empty());

    std::vector<uint8_t> got;
    for (int rounds = 0; rounds < 200 && got.size() < all.size(); rounds++) {
        std::vector<uint8_t> part = drain(fd_server);
        got.insert(got.end(), part.begin(), part.end());
        queue->wait_and_process();
    }
    ASSERT_EQ(got.size(), all.size());
    EXPECT_EQ(got, all);
    EXPECT_NE(element->state, SCK_ERROR);

    delete queue;
    close(fd_server);
}

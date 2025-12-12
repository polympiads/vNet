
#include "vnet/protocol/header.hpp"

#include <gtest/gtest.h>

using namespace vnet::protocol;

TEST(ProtocolHeader, TestConstructorAndGetters) {
    PacketHeader header;
    EXPECT_EQ(header.get_packet_type (), (PacketType) 0);
    EXPECT_EQ(header.get_payload_size(), 0);

    header = PacketHeader(0x12345678, (PacketType) 0x4321);
    EXPECT_EQ(header.get_packet_type (), (PacketType) 0x4321);
    EXPECT_EQ(header.get_payload_size(), 0x12345678);
}

TEST(ProtocolHeader, TestOrdering) {
    unsigned char buffer[PACKET_HEADER_SIZE];

    *((PacketHeader*) buffer) = PacketHeader(0x12345678, (PacketType) 0x4321);

    uint32_t payload_size = *((uint32_t*) buffer);
    uint16_t packet_type  = *((uint16_t*) (buffer + sizeof(uint32_t))); 

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    EXPECT_EQ(payload_size, 0x78563412);
    EXPECT_EQ(packet_type,  0x2143);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    EXPECT_EQ(payload_size, 0x12345678);
    EXPECT_EQ(packet_type,  0x4321);
#endif
}

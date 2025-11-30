
#include "vnet/protocol/types.hpp"

#include <gtest/gtest.h>

using namespace vnet::protocol;

TEST(ProtocolTypes, HostToNetwork) {
    PacketType pkt = (PacketType) 0x0FEF;
    
    uint_packet_t pkt_tp = hton_packet_type(pkt);

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    EXPECT_EQ(pkt_tp, 0xEF0F);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    EXPECT_EQ(pkt_tp, 0x0FEF);
#endif
}

TEST(ProtocolTypes, NetworkToHost) {
    uint_packet_t pkt_tp = 0x0FEF;
    
    uint_packet_t pkt = ntoh_packet_type(pkt_tp);

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    EXPECT_EQ(pkt, 0xEF0F);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    EXPECT_EQ(pkt, 0x0FEF);
#endif
}

#include <gtest/gtest.h>
#include "vnet/blackbox/ipv4.hpp"

#include <cstring>
#include <arpa/inet.h>

using namespace vnet::blackbox;

// Helper: build a minimal valid IPv4+TCP packet
static std::vector<uint8_t> make_tcp_packet(
    const char* src_ip, uint16_t src_port,
    const char* dst_ip, uint16_t dst_port)
{
    // 20 bytes IP header + 4 bytes (ports only) of TCP
    std::vector<uint8_t> pkt(24, 0);

    pkt[0] = 0x45;  // version=4, ihl=5
    uint16_t total = htons(24);
    std::memcpy(pkt.data() + 2, &total, 2);
    pkt[9] = PROTO_TCP;

    uint32_t sip, dip;
    inet_pton(AF_INET, src_ip, &sip);
    inet_pton(AF_INET, dst_ip, &dip);
    std::memcpy(pkt.data() + 12, &sip, 4);
    std::memcpy(pkt.data() + 16, &dip, 4);

    uint16_t sp = htons(src_port);
    uint16_t dp = htons(dst_port);
    std::memcpy(pkt.data() + 20, &sp, 2);
    std::memcpy(pkt.data() + 22, &dp, 2);

    return pkt;
}

TEST(IPv4Parser, ValidTCPPacket) {
    auto pkt = make_tcp_packet("10.0.1.1", 12345, "10.0.1.2", 80);

    IPv4Header hdr{};
    ASSERT_TRUE(ipv4_parse(pkt.data(), pkt.size(), hdr));

    EXPECT_EQ(hdr.version, 4);
    EXPECT_EQ(hdr.ihl, 5);
    EXPECT_EQ(hdr.protocol, PROTO_TCP);

    char src_str[INET_ADDRSTRLEN], dst_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &hdr.src_ip, src_str, sizeof(src_str));
    inet_ntop(AF_INET, &hdr.dst_ip, dst_str, sizeof(dst_str));
    EXPECT_STREQ(src_str, "10.0.1.1");
    EXPECT_STREQ(dst_str, "10.0.1.2");

    EXPECT_EQ(ntohs(hdr.src_port), 12345);
    EXPECT_EQ(ntohs(hdr.dst_port), 80);
}

TEST(IPv4Parser, NullBuffer) {
    IPv4Header hdr{};
    EXPECT_FALSE(ipv4_parse(nullptr, 100, hdr));
}

TEST(IPv4Parser, TooShort) {
    uint8_t buf[10] = {};
    IPv4Header hdr{};
    EXPECT_FALSE(ipv4_parse(buf, sizeof(buf), hdr));
}

TEST(IPv4Parser, WrongVersion) {
    auto pkt = make_tcp_packet("10.0.0.1", 1, "10.0.0.2", 2);
    pkt[0] = 0x65;  // version=6
    IPv4Header hdr{};
    EXPECT_FALSE(ipv4_parse(pkt.data(), pkt.size(), hdr));
}

TEST(IPv4Parser, IHLTooSmall) {
    auto pkt = make_tcp_packet("10.0.0.1", 1, "10.0.0.2", 2);
    pkt[0] = 0x43;  // version=4, ihl=3
    IPv4Header hdr{};
    EXPECT_FALSE(ipv4_parse(pkt.data(), pkt.size(), hdr));
}

TEST(IPv4Parser, TotalLengthExceedsBuffer) {
    auto pkt = make_tcp_packet("10.0.0.1", 1, "10.0.0.2", 2);
    // Set total_length to 1000, buffer is only 24
    uint16_t big = htons(1000);
    std::memcpy(pkt.data() + 2, &big, 2);
    IPv4Header hdr{};
    EXPECT_FALSE(ipv4_parse(pkt.data(), pkt.size(), hdr));
}

TEST(IPv4Parser, UDPPacket) {
    auto pkt = make_tcp_packet("10.0.0.1", 5353, "10.0.0.2", 53);
    pkt[9] = PROTO_UDP;
    IPv4Header hdr{};
    ASSERT_TRUE(ipv4_parse(pkt.data(), pkt.size(), hdr));
    EXPECT_EQ(hdr.protocol, PROTO_UDP);
    EXPECT_EQ(ntohs(hdr.src_port), 5353);
    EXPECT_EQ(ntohs(hdr.dst_port), 53);
}

TEST(IPv4Parser, ICMPNoPorts) {
    // ICMP: 20 byte IP header, no transport ports needed
    std::vector<uint8_t> pkt(20, 0);
    pkt[0] = 0x45;
    uint16_t total = htons(20);
    std::memcpy(pkt.data() + 2, &total, 2);
    pkt[9] = PROTO_ICMP;

    uint32_t sip, dip;
    inet_pton(AF_INET, "10.0.0.1", &sip);
    inet_pton(AF_INET, "10.0.0.2", &dip);
    std::memcpy(pkt.data() + 12, &sip, 4);
    std::memcpy(pkt.data() + 16, &dip, 4);

    IPv4Header hdr{};
    ASSERT_TRUE(ipv4_parse(pkt.data(), pkt.size(), hdr));
    EXPECT_EQ(hdr.protocol, PROTO_ICMP);
    EXPECT_EQ(hdr.src_port, 0);
    EXPECT_EQ(hdr.dst_port, 0);
}

TEST(IPv4Parser, TCPMissingTransportHeader) {
    // Only 20 bytes (IP header), but protocol is TCP → needs 4 more for ports
    std::vector<uint8_t> pkt(20, 0);
    pkt[0] = 0x45;
    uint16_t total = htons(20);
    std::memcpy(pkt.data() + 2, &total, 2);
    pkt[9] = PROTO_TCP;

    IPv4Header hdr{};
    EXPECT_FALSE(ipv4_parse(pkt.data(), pkt.size(), hdr));
}

#pragma once

#include <cstdint>
#include <cstddef>

namespace vnet::blackbox {

    /**
     * Parsed fields from an IPv4 packet header.
     *
     * All multi-byte fields are stored in network byte order
     * (big endian) so they can be compared directly against
     * addresses stored elsewhere in the system without conversion.
     *
     * Ports are only meaningful for TCP (protocol 6) and
     * UDP (protocol 17). For other protocols (e.g. ICMP)
     * they are set to 0.
     */
    struct IPv4Header {
        uint8_t  version;
        uint8_t  ihl;              // header length in 32-bit words (>= 5)
        uint16_t total_length;     // network order
        uint8_t  protocol;         // 1=ICMP, 6=TCP, 17=UDP
        uint32_t src_ip;           // network order
        uint32_t dst_ip;           // network order
        uint16_t src_port;         // network order (0 if not TCP/UDP)
        uint16_t dst_port;         // network order (0 if not TCP/UDP)
    };

    static constexpr uint8_t PROTO_ICMP = 1;
    static constexpr uint8_t PROTO_TCP  = 6;
    static constexpr uint8_t PROTO_UDP  = 17;

    /**
     * Parse a raw IPv4 packet and populate `out`.
     *
     * Validates:
     *  - Minimum buffer size (20 bytes)
     *  - IP version == 4
     *  - IHL >= 5
     *  - Total length consistent with buffer size
     *  - Enough bytes for transport header ports (if TCP/UDP)
     *
     * @param data  Pointer to the raw IPv4 packet.
     * @param len   Number of bytes available in `data`.
     * @param out   Receives the parsed header fields.
     * @return true if the packet is well-formed, false otherwise.
     */
    bool ipv4_parse(const uint8_t* data, size_t len, IPv4Header& out);

};

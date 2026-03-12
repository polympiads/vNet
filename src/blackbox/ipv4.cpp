#include "vnet/blackbox/ipv4.hpp"

#include <cstring>

namespace vnet::blackbox {

    bool ipv4_parse(const uint8_t* data, size_t len, IPv4Header& out) {
        // Minimum IPv4 header is 20 bytes
        if (!data || len < 20)
            return false;

        out.version = (data[0] >> 4) & 0x0F;
        out.ihl     = data[0] & 0x0F;

        if (out.version != 4)
            return false;
        if (out.ihl < 5)
            return false;

        size_t ip_header_bytes = static_cast<size_t>(out.ihl) * 4;
        if (len < ip_header_bytes)
            return false;

        // All multi-byte fields are stored in network byte order (memcpy).
        std::memcpy(&out.total_length, data + 2, 2);
        out.protocol = data[9];

        // total_length is network order — convert to compare against buffer size
        uint16_t total_host = static_cast<uint16_t>(
            (data[2] << 8) | data[3]);
        if (total_host > len)
            return false;

        // Source and destination IPs — network order
        std::memcpy(&out.src_ip, data + 12, 4);
        std::memcpy(&out.dst_ip, data + 16, 4);

        // Extract transport-layer ports for TCP and UDP — network order
        out.src_port = 0;
        out.dst_port = 0;

        if (out.protocol == PROTO_TCP || out.protocol == PROTO_UDP) {
            // Need at least 4 bytes of transport header after the IP header
            if (len < ip_header_bytes + 4)
                return false;

            const uint8_t* transport = data + ip_header_bytes;
            std::memcpy(&out.src_port, transport, 2);
            std::memcpy(&out.dst_port, transport + 2, 2);
        }

        return true;
    }

};

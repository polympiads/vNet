
#pragma once

#include <cstdint>
#include <cstddef>

#include "vnet/protocol/types.hpp"

namespace vnet::protocol {
    #pragma pack(push, 1)
    struct PacketHeader {
    private:
        uint32_t      payload_size;
        uint_packet_t packet_type;
    public:
        uint32_t   get_payload_size();
        PacketType get_packet_type ();

        PacketHeader ();
        PacketHeader (uint32_t payload_size, PacketType packet_type);
    };
    #pragma pack(pop)

    const size_t PACKET_HEADER_SIZE = sizeof(PacketHeader);
};

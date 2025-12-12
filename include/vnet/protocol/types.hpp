
#pragma once

#include <cstdint>

namespace vnet::protocol {
    using uint_packet_t = uint16_t;

    enum PacketType : uint16_t {

    };

    PacketType    ntoh_packet_type (uint_packet_t type);
    uint_packet_t hton_packet_type (PacketType    type);
};

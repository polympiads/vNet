#pragma once

#include <cstdint>

namespace vnet::protocol {
    using uint_packet_t = uint16_t;

    enum PacketType : uint16_t {
        HEARTBEAT = 0,

        // MIP
        SWITCH_MIP,
        AGENT_MIP,

        CONNECT_TO_SWITCH,
        AGENT_CONNECTION_TOKEN,
        AUTH_CONNECT_TO_SWITCH,
        CONNECTION_ACCEPTED,

        // Reconnection
        AGENT_MRP,

        // Routing
        PREPARE_ROUTE_FOR_TARGET,
        NEXT_FOR_TARGET,
        IPV4_RAW
    };

    PacketType    ntoh_packet_type (uint_packet_t type);
    uint_packet_t hton_packet_type (PacketType    type);
};

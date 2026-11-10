#pragma once

#include <cstdint>

namespace vnet::protocol {
    using uint_packet_t = uint16_t;

    /*
     * Wire values are fixed: a type keeps its number for as long as the
     * protocol lives, and a removed type's number is not reused.
     */
    enum PacketType : uint16_t {
        HEARTBEAT              = 0,

        // Machine initialization: registration with the conductor and
        // the token handshake with a switch.
        SWITCH_MIP             = 1,
        AGENT_MIP              = 2,
        CONNECT_TO_SWITCH      = 3,
        AGENT_CONNECTION_TOKEN = 4,
        AUTH_CONNECT_TO_SWITCH = 5,
        CONNECTION_ACCEPTED    = 6,

        // Control-plane reconnection of an agent that kept its switch.
        AGENT_MRP              = 7,

        // 8 and 9 were route requests that were never sent.

        // Data and routes.
        IPV4_RAW               = 10,
        AGENT_REGISTERED       = 11,
        SWITCH_ROUTE_UPDATE    = 12,
        SWITCH_DISCONNECTED    = 13,

        // Peer links, route withdrawal, pair tokens, config fingerprints.
        SWITCH_HELLO           = 14,
        AGENT_UNREGISTERED     = 15,
        PEER_TOKEN             = 16,
        SWITCH_CONFIG          = 17,

        // The switch config itself, conductor to switch.
        CONFIG_PUSH            = 18,
    };

    PacketType    ntoh_packet_type (uint_packet_t type);
    uint_packet_t hton_packet_type (PacketType    type);
};


#pragma once

#include <cstdint>
#include <cstddef>

#include "vnet/const.hpp"
#include "vnet/protocol/types.hpp"

namespace vnet::protocol {
    #pragma pack(push, 1)
    struct PacketHeader {
    private:
        uint32_t      payload_size;
        uint_packet_t packet_type;
    public:
        uint32_t   get_payload_size() const;
        PacketType get_packet_type () const;

        PacketHeader ();
        PacketHeader (uint32_t payload_size, PacketType packet_type);
    };
    #pragma pack(pop)

    const size_t PACKET_HEADER_SIZE = sizeof(PacketHeader);

    /**
     * Largest payload a peer may announce in a header. A packet must fit
     * in a NetworkElement receive buffer, so anything larger is a protocol
     * error and the connection is dropped.
     */
    const uint32_t MAX_PAYLOAD_SIZE = NET_BUFFER_SIZE - PACKET_HEADER_SIZE;
};

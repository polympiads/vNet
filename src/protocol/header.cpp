
#include <arpa/inet.h>
#include "vnet/protocol/header.hpp"
#include "vnet/protocol/types.hpp"

using namespace vnet::protocol;

uint32_t PacketHeader::get_payload_size () const {
    return ntohl(payload_size);
}
PacketType PacketHeader::get_packet_type () const {
    return ntoh_packet_type(packet_type);
}

PacketHeader::PacketHeader () {
    this->payload_size = 0;
    this->packet_type  = 0;
}
PacketHeader::PacketHeader (uint32_t payload_size, PacketType packet_type) {
    this->payload_size = htonl(payload_size);
    this->packet_type  = hton_packet_type(packet_type);
}

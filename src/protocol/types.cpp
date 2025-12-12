
#include "vnet/protocol/types.hpp"

#include <arpa/inet.h>

using namespace vnet::protocol;

PacketType vnet::protocol::ntoh_packet_type (uint_packet_t type) {
    return (PacketType) ntohs(type);
}
uint_packet_t vnet::protocol::hton_packet_type (PacketType type) {
    return htons(type);
}

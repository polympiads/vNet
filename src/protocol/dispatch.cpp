#include "vnet/protocol/dispatch.hpp"

using namespace vnet::protocol;
using namespace vnet::netqueue;

// ---------------------------------------------------------------------------
//  Static C-style callbacks that forward to the Dispatch virtual methods.
//  These are used by makeNetworkQueueHandler to bridge the C function-pointer
//  based NetworkQueueHandler to the Dispatch class hierarchy.
// ---------------------------------------------------------------------------

static void _dispatch_on_close(void* ptr_data, close_data data) {
    static_cast<Dispatch*>(ptr_data)->onClose(data);
}
static void _dispatch_on_socket_ready(void* ptr_data, socket_data data) {
    static_cast<Dispatch*>(ptr_data)->onSocketReady(data);
}
static void _dispatch_on_tun_ready(void* ptr_data, tun_data data) {
    static_cast<Dispatch*>(ptr_data)->onTunReady(data);
}

NetworkQueueHandler vnet::protocol::makeNetworkQueueHandler(Dispatch* dispatch) {
    NetworkQueueHandler handler;
    handler.ptr_data      = dispatch;
    handler.onClose       = _dispatch_on_close;
    handler.onSocketReady = _dispatch_on_socket_ready;
    handler.onTunReady    = _dispatch_on_tun_ready;
    return handler;
}

// ---------------------------------------------------------------------------
//  Packet dispatch — routes an incoming socket_data to the correct
//  typed virtual handler based on the packet_type field.
// ---------------------------------------------------------------------------

void Dispatch::onSocketReady(socket_data data) {
    switch (data.packet_type) {
        case HEARTBEAT: {
            onHeartbeat(data);
            break;
        }
        case SWITCH_MIP: {
            mip::PacketSwitchMIP packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onSwitchMIP(data, packet);
            }
            break;
        }
        case AGENT_MIP: {
            mip::PacketAgentMIP packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onAgentMIP(data, packet);
            }
            break;
        }
        case CONNECT_TO_SWITCH: {
            mip::PacketConnectToSwitch packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onConnectToSwitch(data, packet);
            }
            break;
        }
        case AGENT_CONNECTION_TOKEN: {
            mip::PacketAgentConnectionToken packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onAgentConnectionToken(data, packet);
            }
            break;
        }
        case AUTH_CONNECT_TO_SWITCH: {
            mip::PacketAuthConnectToSwitch packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onAuthConnectToSwitch(data, packet);
            }
            break;
        }
        case CONNECTION_ACCEPTED: {
            mip::PacketConnectionAccepted packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onConnectionAccepted(data, packet);
            }
            break;
        }
        case AGENT_MRP: {
            mip::PacketAgentMRP packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onAgentMRP(data, packet);
            }
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
//  Default (no-op) implementations of all virtual handlers
// ---------------------------------------------------------------------------

void Dispatch::onClose       (close_data)                                          {}
void Dispatch::onTunReady    (tun_data)                                            {}
void Dispatch::onHeartbeat   (socket_data)                                         {}

void Dispatch::onSwitchMIP   (socket_data, mip::PacketSwitchMIP&)                 {}
void Dispatch::onAgentMIP    (socket_data, mip::PacketAgentMIP&)                   {}

void Dispatch::onConnectToSwitch     (socket_data, mip::PacketConnectToSwitch&)    {}
void Dispatch::onAuthConnectToSwitch (socket_data, mip::PacketAuthConnectToSwitch&){}
void Dispatch::onAgentConnectionToken(socket_data, mip::PacketAgentConnectionToken&){}
void Dispatch::onConnectionAccepted  (socket_data, mip::PacketConnectionAccepted&) {}

void Dispatch::onAgentMRP (socket_data, mip::PacketAgentMRP&)                      {}

#include "vnet/protocol/dispatch.hpp"
#include "vnet/protocol/types.hpp"

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
static void _dispatch_on_connected(void* ptr_data, socket_data data) {
    static_cast<Dispatch*>(ptr_data)->onConnected(data);
}

NetworkQueueHandler vnet::protocol::makeNetworkQueueHandler(Dispatch* dispatch) {
    NetworkQueueHandler handler;
    handler.ptr_data      = dispatch;
    handler.onClose       = _dispatch_on_close;
    handler.onSocketReady = _dispatch_on_socket_ready;
    handler.onTunReady    = _dispatch_on_tun_ready;
    handler.onConnected   = _dispatch_on_connected;
    return handler;
}

// ---------------------------------------------------------------------------
//  Packet dispatch — routes an incoming socket_data to the correct
//  typed virtual handler based on the packet_type field.
// ---------------------------------------------------------------------------

void Dispatch::onSocketReady(socket_data data) {
    onActivity(data);
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
        case IPV4_RAW: {
            if (data.payload_size > 0)
                onIPv4Raw(data, data.packet_buffer, data.payload_size);
            break;
        }
        case AGENT_REGISTERED: {
            mip::PacketAgentRegistered packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onAgentRegistered(data, packet);
            }
            break;
        }
        case SWITCH_ROUTE_UPDATE: {
            mip::PacketSwitchRouteUpdate packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onSwitchRouteUpdate(data, packet);
            }
            break;
        }
        case SWITCH_DISCONNECTED: {
            mip::PacketSwitchDisconnected packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onSwitchDisconnected(data, packet);
            }
            break;
        }
        case SWITCH_HELLO: {
            mip::PacketSwitchHello packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onSwitchHello(data, packet);
            }
            break;
        }
        case AGENT_UNREGISTERED: {
            mip::PacketAgentUnregistered packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onAgentUnregistered(data, packet);
            }
            break;
        }
        case PEER_TOKEN: {
            mip::PacketPeerToken packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onPeerToken(data, packet);
            }
            break;
        }
        case SWITCH_CONFIG: {
            mip::PacketSwitchConfig packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onSwitchConfig(data, packet);
            }
            break;
        }
        case CONFIG_PUSH: {
            mip::PacketConfigPush packet;
            if (packet.ParseFromArray(data.packet_buffer, data.payload_size)) {
                onConfigPush(data, packet);
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

void Dispatch::onActivity   (socket_data)                                         {}
void Dispatch::onClose       (close_data)                                          {}
void Dispatch::onTunReady    (tun_data)                                            {}
void Dispatch::onConnected   (socket_data)                                         {}
void Dispatch::onHeartbeat   (socket_data)                                         {}

void Dispatch::onSwitchMIP   (socket_data, mip::PacketSwitchMIP&)                 {}
void Dispatch::onAgentMIP    (socket_data, mip::PacketAgentMIP&)                   {}

void Dispatch::onConnectToSwitch     (socket_data, mip::PacketConnectToSwitch&)    {}
void Dispatch::onAuthConnectToSwitch (socket_data, mip::PacketAuthConnectToSwitch&){}
void Dispatch::onAgentConnectionToken(socket_data, mip::PacketAgentConnectionToken&){}
void Dispatch::onConnectionAccepted  (socket_data, mip::PacketConnectionAccepted&) {}

void Dispatch::onAgentMRP (socket_data, mip::PacketAgentMRP&)                      {}

void Dispatch::onIPv4Raw               (socket_data, const uint8_t*, size_t)           {}
void Dispatch::onAgentRegistered       (socket_data, mip::PacketAgentRegistered&)       {}
void Dispatch::onSwitchRouteUpdate     (socket_data, mip::PacketSwitchRouteUpdate&)     {}
void Dispatch::onSwitchDisconnected    (socket_data, mip::PacketSwitchDisconnected&)    {}
void Dispatch::onSwitchHello           (socket_data, mip::PacketSwitchHello&)           {}
void Dispatch::onAgentUnregistered     (socket_data, mip::PacketAgentUnregistered&)     {}
void Dispatch::onPeerToken             (socket_data, mip::PacketPeerToken&)             {}
void Dispatch::onSwitchConfig          (socket_data, mip::PacketSwitchConfig&)          {}
void Dispatch::onConfigPush            (socket_data, mip::PacketConfigPush&)            {}

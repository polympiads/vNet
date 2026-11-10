#pragma once

#include "vnet/netqueue/handler.hpp"
#include <cstdint>
#include <cstddef>
#include "mip.pb.h"

namespace vnet::protocol {

    struct Dispatch {
    public:
        void onSocketReady (netqueue::socket_data data);

        virtual void onActivity  (netqueue::socket_data data);
        virtual void onClose     (netqueue::close_data close_content);
        virtual void onTunReady  (netqueue::tun_data   data);
        /** A pending connect or TLS handshake finished; packets can flow. */
        virtual void onConnected (netqueue::socket_data data);

        virtual void onHeartbeat (netqueue::socket_data data);

        /** Machine Initialization Procedure */
        virtual void onSwitchMIP (netqueue::socket_data data, mip::PacketSwitchMIP &packet);
        virtual void onAgentMIP  (netqueue::socket_data data, mip::PacketAgentMIP  &packet);

        virtual void onConnectToSwitch      (netqueue::socket_data data, mip::PacketConnectToSwitch      &packet);
        virtual void onAuthConnectToSwitch  (netqueue::socket_data data, mip::PacketAuthConnectToSwitch  &packet);
        virtual void onAgentConnectionToken (netqueue::socket_data data, mip::PacketAgentConnectionToken &packet);
        virtual void onConnectionAccepted   (netqueue::socket_data data, mip::PacketConnectionAccepted   &packet);

        /** Reconnection */
        virtual void onAgentMRP (netqueue::socket_data data, mip::PacketAgentMRP &packet);

        /** Routing */
        /** One IPv4 datagram, as it was read from a TUN: no protobuf wrapper. */
        virtual void onIPv4Raw(netqueue::socket_data data, const uint8_t* raw, size_t len);
        virtual void onAgentRegistered    (netqueue::socket_data data, mip::PacketAgentRegistered    &packet);
        virtual void onSwitchRouteUpdate  (netqueue::socket_data data, mip::PacketSwitchRouteUpdate  &packet);
        virtual void onSwitchDisconnected (netqueue::socket_data data, mip::PacketSwitchDisconnected &packet);
        virtual void onSwitchHello        (netqueue::socket_data data, mip::PacketSwitchHello        &packet);
        virtual void onAgentUnregistered  (netqueue::socket_data data, mip::PacketAgentUnregistered  &packet);
        virtual void onPeerToken          (netqueue::socket_data data, mip::PacketPeerToken          &packet);
        virtual void onSwitchConfig       (netqueue::socket_data data, mip::PacketSwitchConfig       &packet);

        virtual ~Dispatch() = default;
    };

    netqueue::NetworkQueueHandler makeNetworkQueueHandler (Dispatch *dispatch);
};

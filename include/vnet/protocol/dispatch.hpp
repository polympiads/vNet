#pragma once

#include "vnet/netqueue/handler.hpp"
#include "mip.pb.h"

namespace vnet::protocol {

    struct Dispatch {
    public:
        void onSocketReady (netqueue::socket_data data);

        virtual void onClose    (netqueue::close_data close_content);
        virtual void onTunReady (netqueue::tun_data   data);

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
        virtual void onPrepareRouteForTarget(netqueue::socket_data data, mip::PacketPrepareRouteForTarget &packet);
        virtual void onNextForTarget(netqueue::socket_data data, mip::PacketNextForTarget &packet);
        virtual void onIPv4Raw(netqueue::socket_data data, mip::PacketIPv4Raw &packet);

        virtual ~Dispatch() = default;
    };

    netqueue::NetworkQueueHandler makeNetworkQueueHandler (Dispatch *dispatch);
};

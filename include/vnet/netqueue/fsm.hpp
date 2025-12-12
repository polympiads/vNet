
#pragma once

namespace vnet::netqueue {
    /**
     * This finite state machine represents the state in which the file descriptor
     * is currently. In particular, there are two distinct finite state machines
     * depending on the type of socket.
     * 
     * Anything in TUN_* represents a device that operates as a queue of packets,
     * under which reading yields the entire IP packet directly and on which you
     * should only write a entire IP packet.
     * 
     * Anything in SCK_* represents a device that follows the specific protobuf
     * format in the specifications. In particular, it contains first a 6 byte
     * header containing the payload size and payload type.
     */
    enum FiniteStateMachine {
        TUN_RECEIVE,
        TUN_READY,
        TUN_ERROR,
        TUN_EOF,

        SCK_HEADER,
        SCK_PAYLOAD,
        SCK_READY,
        SCK_ERROR,
        SCK_EOF
    };
};

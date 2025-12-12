
#pragma once

#include <cstdint>
#include <cstddef>

#include "vnet/const.hpp"
#include "vnet/netqueue/fsm.hpp"
#include <cstdlib>

namespace vnet::netqueue {
    struct NetworkElement {
        FiniteStateMachine state;

        int fd;
        void *ptr;

        size_t net_buffer_used;
        uint8_t net_buffer[NET_BUFFER_SIZE];
    
        NetworkElement (int fd, void *ptr, FiniteStateMachine state);
    };
}

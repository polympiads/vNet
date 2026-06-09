
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <vector>

#include "vnet/const.hpp"
#include "vnet/netqueue/fsm.hpp"

namespace vnet::netqueue {

    static constexpr size_t WRITE_BUFFER_MAX = 65536; // 64KB

    struct NetworkElement {
        FiniteStateMachine state;

        int fd;
        void *ptr;

        size_t net_buffer_used;
        uint8_t net_buffer[NET_BUFFER_SIZE];

        std::vector<uint8_t> write_buffer;
        size_t               write_buffer_offset = 0;
    
        NetworkElement (int fd, void *ptr, FiniteStateMachine state);
    };
}

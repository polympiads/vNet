
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <vector>

#include "vnet/const.hpp"
#include "vnet/netqueue/fsm.hpp"

/* OpenSSL session type, declared here so no copy of it lands in this namespace. */
struct ssl_st;

namespace vnet::netqueue {

    /* Bytes a peer may have waiting on a slow socket before the queue
     * stops taking data for it: data packets are dropped, control packets
     * close the link. A peer link carries many agents, so this is well
     * above one 64 KB packet. */
    static constexpr size_t WRITE_BUFFER_MAX = 2 * 1024 * 1024;

    struct NetworkElement {
        FiniteStateMachine state;

        int fd;
        void *ptr;

        size_t net_buffer_used;
        uint8_t net_buffer[NET_BUFFER_SIZE];

        std::vector<uint8_t> write_buffer;
        size_t               write_buffer_offset = 0;

        /* Set once close() has taken the element. The object stays alive
         * until the current epoll batch finishes. */
        bool dead = false;

        /* Optional TLS session. Null means the fd is plaintext. */
        ssl_st* ssl = nullptr;

        /* A handshake step asked for the socket to become writable. */
        bool want_write = false;

        /* Set when process() stopped at the per-turn packet cap with input
         * still to read; the queue comes back to it next turn. */
        bool has_backlog = false;
        /* The last turn of wait_and_process that handled this element. */
        uint64_t last_turn = 0;
    
        NetworkElement (int fd, void *ptr, FiniteStateMachine state);
    };
}


#pragma once

#include "vnet/netqueue/element.hpp"
#include "vnet/protocol/header.hpp"

namespace vnet::netqueue {
    enum close_reason {
        CLOSE_ERROR,
        CLOSE_EPOLL,
        CLOSE_HANGUP
    };
    struct close_data {
        NetworkElement* net_element;

        void* ptr_data;
        int   fd;

        close_reason reason;
    };
    struct socket_data {
        NetworkElement* net_element;
        
        void* ptr_data;
        int   fd;

        protocol::PacketType packet_type;
        unsigned char* packet_buffer;
        size_t payload_size;

        unsigned char* full_buffer;
        size_t full_buffer_size;
    };
    struct tun_data {
        NetworkElement* net_element;
        
        void* ptr_data;
        int   fd;
        
        unsigned char* ip_buffer;
        size_t ip_buffer_size;
    };

    void doNothingOnClose       (void* ptr_data, close_data  close_content);
    void doNothingOnSocketReady (void* ptr_data, socket_data data);
    void doNothingOnTunReady    (void* ptr_data, tun_data    data);

    struct NetworkQueueHandler {
        void (*onClose)       (void* ptr_data, close_data  close_content) = &doNothingOnClose;
        void (*onSocketReady) (void* ptr_data, socket_data data)          = &doNothingOnSocketReady;
        void (*onTunReady)    (void* ptr_data, tun_data    data)          = &doNothingOnTunReady;
        
        void* ptr_data = nullptr;
    };

}

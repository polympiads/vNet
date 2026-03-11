
#include "vnet/netqueue/handler.hpp"
#include <vector>

using namespace vnet::netqueue;

std::vector<socket_data> socket_datas;
std::vector<close_data>  close_datas;
std::vector<tun_data>    tun_datas;

void onSocketReady (void* ptr_data, socket_data data) {
    socket_datas.push_back(data);
}
void onTunReady (void* ptr_data, tun_data data) {
    tun_datas.push_back(data);
}
void onClose (void* ptr_data, close_data data) {
    close_datas.push_back(data);
}

NetworkQueueHandler createTestHandler () {
    NetworkQueueHandler handler;
    handler.onSocketReady = &onSocketReady;
    handler.onTunReady    = &onTunReady;
    handler.onClose       = &onClose;
    handler.ptr_data      = nullptr; // unused in this test

    return handler;
}


#include "vnet/netqueue/handler.hpp"

using namespace vnet::netqueue;

void vnet::netqueue::doNothingOnClose       (void* ptr_data, close_data    error_content) {}
void vnet::netqueue::doNothingOnSocketReady (void* ptr_data, socket_data data) {}
void vnet::netqueue::doNothingOnTunReady    (void* ptr_data, tun_data    data) {}

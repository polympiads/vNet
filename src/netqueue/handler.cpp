
#include "vnet/netqueue/handler.hpp"

using namespace vnet::netqueue;

void vnet::netqueue::doNothingOnClose       (close_data    error_content) {}
void vnet::netqueue::doNothingOnSocketReady (socket_data data) {}
void vnet::netqueue::doNothingOnTunReady    (tun_data    data) {}

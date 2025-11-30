
#include "vnet/netqueue/element.hpp"

using namespace vnet::netqueue;

NetworkElement::NetworkElement (int fd, void *ptr, FiniteStateMachine state) {
    this->net_buffer_used = 0;

    this->fd    = fd;
    this->ptr   = ptr;
    this->state = state;
}

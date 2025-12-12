
#include "netqueue/netqueue_macro.hpp"

using namespace vnet::netqueue;

TEST(NetQueueElement, CreateNetworkElement) {
    NetworkElement* element = new NetworkElement( 0, NULL, SCK_HEADER );
    
    EXPECT_NE(element, nullptr);

    EXPECT_EQ(element->fd,              0);
    EXPECT_EQ(element->ptr,             nullptr);
    EXPECT_EQ(element->state,           SCK_HEADER);
    EXPECT_EQ(element->net_buffer_used, 0);

    delete element;
}

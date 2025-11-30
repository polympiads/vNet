
#include "netqueue/netqueue_macro.hpp"

using namespace vnet::netqueue;
using namespace vnet::protocol;

TEST(NetQueue, InvalidFSM) {
    PREPARE_NETWORK_TEST((FiniteStateMachine) 123);

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);

    EXPECT_EQ(element->state, (FiniteStateMachine) 123);
    
    delete queue;
    delete element;
    close(fd_server);
    close(fd_client);
}
TEST(NetQueue, InvalidErrorHandler) {
    NetworkQueueHandler handler;
    handler.onClose = nullptr;

    EXPECT_THROW(new NetQueue(handler), std::invalid_argument);
    
    handler = NetworkQueueHandler();
    handler.onSocketReady = nullptr;
    EXPECT_THROW(new NetQueue(handler), std::invalid_argument);
    
    handler = NetworkQueueHandler();
    handler.onTunReady = nullptr;
    EXPECT_THROW(new NetQueue(handler), std::invalid_argument);
}

TEST(NetQueue, GetNetworkElementInvalidFD) {
    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    EXPECT_EQ( queue->get_network_element_from_fd(25), nullptr );
    delete queue;
}
TEST(NetQueue, GetNetworkElement) {
    PREPARE_SOCKET_PAIR
    
    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    NetworkElement* element = queue->put_sck( fd_client, nullptr );

    EXPECT_NE(element, nullptr);
    EXPECT_EQ(element, queue->get_network_element_from_fd(fd_client));
    
    close(fd_server);
    delete queue;
}
TEST(NetQueue, CloseNetworkElement) {
    PREPARE_SOCKET_PAIR

    unsigned char bf[1];

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    NetworkElement* element = queue->put_sck( fd_client, nullptr );

    EXPECT_EQ( read(fd_client, bf, 1), -1 );
    EXPECT_EQ( errno, EAGAIN );
    EXPECT_EQ( element, queue->get_network_element_from_fd(fd_client) );

    queue->close( nullptr );

    EXPECT_EQ( read(fd_client, bf, 1), -1 );
    EXPECT_EQ( errno, EAGAIN );
    EXPECT_EQ( element, queue->get_network_element_from_fd(fd_client) );
    
    NetworkElement* wrong_element = new NetworkElement(STDIN_FILENO, nullptr, SCK_HEADER);
    queue->close( wrong_element );
    delete wrong_element;

    EXPECT_EQ( read(fd_client, bf, 1), -1 );
    EXPECT_EQ( errno, EAGAIN );
    EXPECT_EQ( element, queue->get_network_element_from_fd(fd_client) );

    queue->close(element);

    EXPECT_EQ( read(fd_client, bf, 1), -1 );
    EXPECT_EQ( errno, EBADF );
    EXPECT_EQ( queue->get_network_element_from_fd(fd_client), nullptr );
    
    close(fd_server);
    delete queue;
}
TEST(NetQueue, CloseFD) {
    PREPARE_SOCKET_PAIR

    unsigned char bf[1];

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    NetworkElement* element = queue->put_sck( fd_client, nullptr );

    EXPECT_EQ( read(fd_client, bf, 1), -1 );
    EXPECT_EQ( errno, EAGAIN );
    EXPECT_EQ( element, queue->get_network_element_from_fd(fd_client) );

    queue->close( fd_client + 1 );

    EXPECT_EQ( read(fd_client, bf, 1), -1 );
    EXPECT_EQ( errno, EAGAIN );
    EXPECT_EQ( element, queue->get_network_element_from_fd(fd_client) );

    queue->close( fd_client );

    EXPECT_EQ( read(fd_client, bf, 1), -1 );
    EXPECT_EQ( errno, EBADF );
    EXPECT_EQ( queue->get_network_element_from_fd(fd_client), nullptr );
    
    close(fd_server);
    delete queue;
}

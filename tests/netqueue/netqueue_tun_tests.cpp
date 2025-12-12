
#include "netqueue/netqueue_macro.hpp"
#include "netqueue/netqueue_test_handler.hpp"

using namespace vnet::netqueue;
using namespace vnet::protocol;

TEST(NetQueueTUN, ProcessReceive) {
    PREPARE_NETWORK_TUN_TEST(TUN_RECEIVE);

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);

    EXPECT_EQ(TUN_RECEIVE, element->state);
    
    delete queue;
    delete element;
    close(fd_server);
    close(fd_client);
}
TEST(NetQueueTUN, ProcessReady) {
    PREPARE_NETWORK_TUN_TEST(TUN_READY);

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);
    
    EXPECT_EQ(TUN_RECEIVE, element->state);

    delete queue;
    delete element;
    close(fd_server);
    close(fd_client);
}
TEST(NetQueueTUN, ProcessError) {
    PREPARE_NETWORK_TUN_TEST(TUN_ERROR);

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);
    
    EXPECT_EQ(TUN_ERROR, element->state);
    
    delete queue;
    delete element;
    close(fd_server);
    close(fd_client);
}
TEST(NetQueueTUN, ProcessEOF) {
    PREPARE_NETWORK_TUN_TEST(TUN_EOF);

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);
    
    EXPECT_EQ(TUN_EOF, element->state);
    
    delete queue;
    delete element;
    close(fd_server);
    close(fd_client);
}

TEST(NetQueueTUN, FullProcess) {
    PREPARE_TUN_PAIR

    NetQueue* queue = new NetQueue(createTestHandler(), 100);
    NetworkElement* element = queue->put_tun(fd_client, nullptr);

    write(fd_server, "sample1", 7);
    queue->wait_and_process();
    
    EXPECT_EQ( (int) tun_datas.size(), 1 );
    EXPECT_EQ( tun_datas[0].fd, fd_client );
    EXPECT_EQ( tun_datas[0].ptr_data, nullptr );
    EXPECT_EQ( tun_datas[0].net_element, element );
    EXPECT_EQ( tun_datas[0].ip_buffer_size, 7 );
    EXPECT_EQ( tun_datas[0].ip_buffer[0], 's' );
    EXPECT_EQ( tun_datas[0].ip_buffer[1], 'a' );
    EXPECT_EQ( tun_datas[0].ip_buffer[2], 'm' );
    EXPECT_EQ( tun_datas[0].ip_buffer[3], 'p' );
    EXPECT_EQ( tun_datas[0].ip_buffer[4], 'l' );
    EXPECT_EQ( tun_datas[0].ip_buffer[5], 'e' );
    EXPECT_EQ( tun_datas[0].ip_buffer[6], '1' );
    
    write(fd_server, "sample23", 8);
    queue->wait_and_process();
    
    EXPECT_EQ( (int) tun_datas.size(), 2 );
    EXPECT_EQ( tun_datas[1].fd, fd_client );
    EXPECT_EQ( tun_datas[1].ptr_data, nullptr );
    EXPECT_EQ( tun_datas[1].net_element, element );
    EXPECT_EQ( tun_datas[1].ip_buffer_size, 8 );
    EXPECT_EQ( tun_datas[1].ip_buffer[0], 's' );
    EXPECT_EQ( tun_datas[1].ip_buffer[1], 'a' );
    EXPECT_EQ( tun_datas[1].ip_buffer[2], 'm' );
    EXPECT_EQ( tun_datas[1].ip_buffer[3], 'p' );
    EXPECT_EQ( tun_datas[1].ip_buffer[4], 'l' );
    EXPECT_EQ( tun_datas[1].ip_buffer[5], 'e' );
    EXPECT_EQ( tun_datas[1].ip_buffer[6], '2' );
    EXPECT_EQ( tun_datas[1].ip_buffer[7], '3' );

    delete queue;
    close(fd_server);
}
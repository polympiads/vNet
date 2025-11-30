
#include "netqueue/netqueue_macro.hpp"
#include "netqueue/netqueue_test_handler.hpp"

using namespace vnet::netqueue;
using namespace vnet::protocol;

TEST(NetQueueEpoll, SetupEPOLL) {
    NetQueue* queue = new NetQueue(NetworkQueueHandler(), 500);
    queue->wait_and_process();
    
    delete queue;
}
TEST(NetQueueEpoll, ProcessFullAsEpoll) {
    uint8_t buffer[24];
    PacketHeader header(0x12, (PacketType) 0x32);
    memcpy( buffer, &header, sizeof(PacketHeader));
    memcpy( buffer + sizeof(PacketHeader), "Hello, SCK World !", 0x12 );

    std::vector<std::vector<std::pair<FiniteStateMachine, int>>> cuts_lists = {
        { { SCK_HEADER, 24 } },
        { { SCK_PAYLOAD, 16 }, { SCK_HEADER, 8 } },
        { { SCK_HEADER, 4 }, { SCK_PAYLOAD, 4 }, { SCK_PAYLOAD, 8 }, { SCK_HEADER, 8 } },
        { { SCK_HEADER, 4 }, { SCK_PAYLOAD, 13 }, { SCK_HEADER, 7 } }
    };

    NetQueue* queue = new NetQueue(NetworkQueueHandler(), 500);

    for (const auto &cuts : cuts_lists) {
        PREPARE_SOCKET_PAIR
        NetworkElement* element = queue->put_sck(fd_client, NULL);
        std::cout << "========" << std::endl;

        //queue.wait_and_process();
        
        size_t sent = 0;
        for (auto [final_state, to_send] : cuts) {
            std::cout << final_state << " " << to_send << std::endl;

            write( fd_server, buffer + sent, to_send );
            sent += to_send;

            queue->wait_and_process();

            EXPECT_EQ(element->state, final_state);
            if (sent != 24) EXPECT_EQ(element->net_buffer_used, sent);
            else EXPECT_EQ(element->net_buffer_used, 0);

            for (size_t offset = 0; offset < sent; offset ++)
                EXPECT_EQ(element->net_buffer[offset], buffer[offset]);
        }

        close(fd_server);

        std::cout << std::endl;
    }
    
    delete queue;
}
TEST(NetQueueEpoll, ProcessManyAsEpoll) {
    uint8_t buffer[24];
    PacketHeader header(0x12, (PacketType) 0x32);
    memcpy( buffer, &header, sizeof(PacketHeader));
    memcpy( buffer + sizeof(PacketHeader), "Hello, SCK World !", 0x12 );

    const int NUM_BUFFERS = 4;

    NetQueue* queue = new NetQueue(createTestHandler(), 500);

    std::vector<NetworkElement*> elements;
    std::vector<int> fds;
    for (int i = 0; i < NUM_BUFFERS; i ++) {
        PREPARE_SOCKET_PAIR
        NetworkElement* element = queue->put_sck(fd_client, NULL);
        EXPECT_EQ( write(fd_server, buffer, 24), 24 );
        elements.push_back(element);
        fds.push_back(fd_server);
    }

    queue->wait_and_process();

    EXPECT_EQ(socket_datas.size(), NUM_BUFFERS);
    for (auto data : socket_datas) {
        EXPECT_EQ(data.full_buffer_size, 24);
        for (size_t offset = 0; offset < 24; offset ++)
            EXPECT_EQ(data.full_buffer[offset], buffer[offset]);
        for (size_t offset = 0; offset < 18; offset ++)
            EXPECT_EQ(data.packet_buffer[offset], buffer[offset + 6]);
        EXPECT_EQ(data.packet_type, (PacketType) 0x32);
        EXPECT_EQ(data.ptr_data, nullptr);
    }

    for (int fd : fds) close(fd);

    delete queue;
}
TEST(NetQueueEpoll, NetQueuePutError) {
    NetQueue* queue = new NetQueue(NetworkQueueHandler(), 500);
    NetworkElement* element = queue->put(-1, nullptr, SCK_HEADER);
    EXPECT_EQ(element, nullptr);
    EXPECT_EQ(errno, EBADF);

    delete queue;
}

TEST(NetQueueEpoll, NetQueueWaitError) {
    NetQueue* queue = new NetQueue(NetworkQueueHandler(), 500);
    uint32_t* buffer = (uint32_t*) queue;
    close(buffer[0]);

    queue->wait_and_process();
    delete queue;
}

TEST(NetQueueSocket, ProcessEOF) {
    PREPARE_SOCKET_PAIR

    NetQueue* queue = new NetQueue(createTestHandler(), 100);
    NetworkElement* element = queue->put_sck( fd_client, nullptr );

    queue->wait_and_process();
    EXPECT_EQ(close_datas.size(), 0);

    close(fd_server);
    
    queue->wait_and_process();
    EXPECT_EQ(close_datas.size(), 1);

    EXPECT_EQ(close_datas.front().net_element, element);
    EXPECT_EQ(close_datas.front().fd, fd_client);
    EXPECT_EQ(close_datas.front().ptr_data, nullptr);
    EXPECT_EQ(close_datas.front().reason, CLOSE_HANGUP);
    
    delete queue;
}

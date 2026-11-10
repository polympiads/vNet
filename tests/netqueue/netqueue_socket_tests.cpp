
#include "netqueue/netqueue_macro.hpp"

using namespace vnet::netqueue;
using namespace vnet::protocol;

TEST(NetQueueSocket, ProcessHeader_Failure) {
    PREPARE_NETWORK_TEST(SCK_HEADER)

    close(fd_client);
    close(fd_server);

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);

    EXPECT_EQ( element->net_buffer_used, 0 );
    EXPECT_EQ( element->state, SCK_ERROR );

    delete queue;
    delete element;
}
TEST(NetQueueSocket, ProcessHeader_PartialSend) {
    PREPARE_NETWORK_TEST(SCK_HEADER)

    PacketHeader header(0x12, (PacketType) 0x32);
    EXPECT_EQ( write(fd_server, &header, 3), 3 );

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);
    
    EXPECT_EQ( element->net_buffer_used, 3 );
    EXPECT_EQ( element->state, SCK_HEADER );
    
    delete queue;
    delete element;
    close(fd_client);
    close(fd_server);
}
TEST(NetQueueSocket, ProcessHeader_Success) {
    PREPARE_NETWORK_TEST(SCK_HEADER)

    PacketHeader header(0x12, (PacketType) 0x32);
    EXPECT_EQ( write(fd_server, &header, sizeof(header)), sizeof(header) );

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);

    EXPECT_EQ( element->net_buffer_used, sizeof(header) );
    EXPECT_EQ( element->state, SCK_PAYLOAD );
    
    delete queue;
    delete element;
    close(fd_client);
    close(fd_server);
}

TEST(NetQueueSocket, ProcessHeader_OversizedPayload) {
    PREPARE_NETWORK_TEST(SCK_HEADER)

    PacketHeader header(MAX_PAYLOAD_SIZE + 1, (PacketType) 0x32);
    EXPECT_EQ( write(fd_server, &header, sizeof(header)), sizeof(header) );

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);

    EXPECT_EQ( element->state, SCK_ERROR );

    delete queue;
    delete element;
    close(fd_client);
    close(fd_server);
}
TEST(NetQueueSocket, ProcessHeader_MaxPayloadAccepted) {
    PREPARE_NETWORK_TEST(SCK_HEADER)

    PacketHeader header(MAX_PAYLOAD_SIZE, (PacketType) 0x32);
    EXPECT_EQ( write(fd_server, &header, sizeof(header)), sizeof(header) );

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);

    EXPECT_EQ( element->net_buffer_used, sizeof(header) );
    EXPECT_EQ( element->state, SCK_PAYLOAD );

    delete queue;
    delete element;
    close(fd_client);
    close(fd_server);
}

TEST(NetQueueSocket, ProcessPayload_Failure) {
    PREPARE_NETWORK_TEST(SCK_PAYLOAD)

    PacketHeader header(0x12, (PacketType) 0x32);
    element->net_buffer_used = sizeof(PacketHeader);
    *((PacketHeader*) element->net_buffer) = header;

    close(fd_client);
    close(fd_server);

    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);
    
    EXPECT_EQ( element->net_buffer_used, 6 );
    EXPECT_EQ( element->state, SCK_ERROR );

    delete queue;
    delete element;
}
TEST(NetQueueSocket, ProcessPayload_PartialSend) {
    PREPARE_NETWORK_TEST(SCK_PAYLOAD)

    PacketHeader header(0x12, (PacketType) 0x32);
    element->net_buffer_used = sizeof(PacketHeader);
    *((PacketHeader*) element->net_buffer) = header;

    const char* to_send = "Hello, SCK World !";
    EXPECT_EQ( write(fd_server, to_send, 12), 12 );
    
    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);
    
    EXPECT_EQ( element->net_buffer_used, 18 );
    EXPECT_EQ( element->state, SCK_PAYLOAD );

    delete queue;
    delete element;
    close(fd_client);
    close(fd_server);
}
TEST(NetQueueSocket, ProcessPayload_Success) {
    PREPARE_NETWORK_TEST(SCK_PAYLOAD)

    PacketHeader header(0x12, (PacketType) 0x32);
    element->net_buffer_used = sizeof(PacketHeader);
    *((PacketHeader*) element->net_buffer) = header;

    const char* to_send = "Hello, SCK World !";
    EXPECT_EQ( write(fd_server, to_send, 18), 18 );
    
    NetQueue* queue = new NetQueue(NetworkQueueHandler());
    queue->process(element);
    
    EXPECT_EQ( element->net_buffer_used, 0 );
    EXPECT_EQ( element->state, SCK_HEADER );

    delete queue;
    delete element;
    close(fd_client);
    close(fd_server);
}

static void rejectOnSocketReady (void*, socket_data data) {
    data.net_element->state = SCK_ERROR;
}

TEST(NetQueueSocket, ProcessReady_HandlerRejectKeepsError) {
    PREPARE_NETWORK_TEST(SCK_HEADER)

    // Two complete packets: the second must not be read once the handler
    // rejected the first.
    PacketHeader header(0, (PacketType) 0x32);
    EXPECT_EQ( write(fd_server, &header, sizeof(header)), sizeof(header) );
    EXPECT_EQ( write(fd_server, &header, sizeof(header)), sizeof(header) );

    NetworkQueueHandler handler;
    handler.onSocketReady = &rejectOnSocketReady;
    NetQueue* queue = new NetQueue(handler);
    queue->process(element);

    EXPECT_EQ( element->state, SCK_ERROR );
    uint8_t rest[sizeof(header)];
    EXPECT_EQ( read(fd_client, rest, sizeof(rest)), (ssize_t) sizeof(rest) );

    delete queue;
    delete element;
    close(fd_client);
    close(fd_server);
}

TEST(NetQueueSocket, ProcessFull_InBlocks) {
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

    NetQueue* queue = new NetQueue(NetworkQueueHandler());

    for (const auto &cuts : cuts_lists) {
        PREPARE_NETWORK_TEST(SCK_HEADER);
        std::cout << "========" << std::endl;

        size_t sent = 0;
        for (auto [final_state, to_send] : cuts) {
            std::cout << final_state << " " << to_send << std::endl;

            write( fd_server, buffer + sent, to_send );
            sent += to_send;

            queue->process(element);

            EXPECT_EQ(element->state, final_state);
            if (sent != 24) EXPECT_EQ(element->net_buffer_used, sent);
            else EXPECT_EQ(element->net_buffer_used, 0);

            for (size_t offset = 0; offset < sent; offset ++)
                EXPECT_EQ(element->net_buffer[offset], buffer[offset]);
        }

        delete element;
        close(fd_client);
        close(fd_server);
    
        std::cout << std::endl;
    }

    delete queue;
}

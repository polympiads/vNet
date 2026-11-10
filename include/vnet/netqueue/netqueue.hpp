
#pragma once

#include "vnet/netqueue/fsm.hpp"
#include "vnet/netqueue/element.hpp"
#include "vnet/netqueue/handler.hpp"
#include "vnet/protocol/types.hpp"

#include <google/protobuf/message.h>
#include <unordered_map>
#include <mutex>
#include <vector>

/* OpenSSL session type, declared here so no copy of it lands in this namespace. */
struct ssl_st;

namespace vnet::netqueue {
    /**
     * Maximum number of events returned by a single call to epoll_wait.
     * A switch with hundreds of agents gets most of them in one turn.
     */
    const int EPOLL_MAX_EVENTS = 256;

    /**
     * Packets one element may deliver in one turn of wait_and_process.
     * A socket that is fed without pause is put down after this many and
     * taken up again next turn, so it cannot starve the other elements.
     */
    const int PACKETS_PER_TURN = 64;

    /**
     * Instances of this class represents a general
     * purpose pool of file descriptors intended to
     * represent a queue for incoming network packets.
     *
     * The process must ignore SIGPIPE: a write to a peer that hung up,
     * including the TLS alert OpenSSL sends on a failed handshake, is
     * reported as an error on that socket instead of ending the process.
     */
    struct NetQueue {
    private:
        /*
         * File descriptor of epoll
         *
         * This file descriptor is owned by the instance.
         */
        int epoll_fd = -1;
        /* Timeout on epoll_wait */
        int epoll_timeout = -1;

        /* 
         * Unordered map with file descriptor linked 
         * to the respective network elements that contain it.
         */
        std::unordered_map<int, NetworkElement*> fd_to_network_element;
        /* Mutex for all operations related to the fd_to_network_element unordered_map */
        std::mutex fd_to_network_element_mutex;

        /* Elements removed from the map but not yet deleted, so an epoll
         * batch that still holds their pointer can skip them safely. */
        std::vector<NetworkElement*> deferred_delete;

        /* Elements that hit PACKETS_PER_TURN with input left: handled
         * first next turn, which then does not sleep in epoll_wait. */
        std::vector<NetworkElement*> backlog;
        uint64_t turn = 0;

        void reap_closed();

        /* One element that epoll (or the backlog) reported ready. */
        void handle_ready(NetworkElement* element, uint32_t events);

        /*
         * Handler of events (e.g. receiving a packet,
         *   error of a specific fd etc...)
         * 
         * The memory of the handler is owned by this object.
         */
        NetworkQueueHandler handler;

        /**
        * Attempt to drain the write buffer for an element.
        * Registers/deregisters EPOLLOUT as needed.
        * Returns false if the connection should be closed.
        */
        bool flush_write_buffer(NetworkElement* element);

        /* Advance a pending connect or handshake. Returns false when the
         * element is not ready yet and process() should stop. */
        bool step_connecting(NetworkElement* element);
        bool step_handshake(NetworkElement* element);
        void fire_connected(NetworkElement* element);

        /**
        * Shared body of send and send_data. With drop_when_full, a write
        * buffer that cannot take n more bytes makes the call return
        * false and leaves the connection alone.
        */
        bool send_impl(int fd, const void* data, size_t n, bool drop_when_full);

    public:

        /**
        * Send data to a file descriptor managed by this queue.
        *
        * If the kernel send buffer is full (EAGAIN), the remaining
        * bytes are saved in the element's write buffer and sent when
        * epoll signals the fd is writable (EPOLLOUT).
        *
        * If the write buffer exceeds WRITE_BUFFER_MAX the connection
        * is closed and onClose is fired.
        *
        * @return true if all bytes were sent or buffered successfully.
        *         false if the fd is not in the queue or the buffer limit
        *         was exceeded (connection closed).
        */
        bool send(int fd, const void* data, size_t n);

        /**
        * Send a protobuf message with the 6-byte packet header
        * through the queue's write path, as one write.
        */
        bool send(int fd, protocol::PacketType type, const google::protobuf::Message& msg);

        /**
        * Send a data packet: a 6-byte header of the given type followed by
        * the bytes as they are. When the peer's write buffer is full the
        * packet is dropped and false is returned; the connection stays
        * open, since the overlay's own transport retransmits.
        */
        bool send_data(int fd, protocol::PacketType type, const void* data, size_t n);

        /**
        * Send a heartbeat (header-only) through the queue's write path.
        */
        bool send_heartbeat(int fd);

        /**
         * Put a file descriptor into the queue
         * with a pointer to some data the user wants
         * to store.
         * 
         * The user GIVES OWNERSHIP of the file descriptor
         * to the network queue. One should never close it 
         * manually. If the user wishes to close the file
         * descriptor, it should use the close method of
         * the network queue.
         * 
         * The user still HAS OWNERSHIP over the pointer of
         * the data it wanted to store and will be responsible
         * for freeing it or handling it upon receival of an
         * error or a close through the NetworkQueueHandler.
         * 
         * The user DOES NOT HAVE OWNERSHIP over the resulting
         * network element and should in no case free it, close 
         * the internal file descriptor or manipulate the state,
         * buffer and number of bytes used in the buffer.
         * 
         * The user MAY modify the network's element internal
         * pointer to some new data it wants to use to handle
         * for example a client reconnecting.
         * 
         * @return the generated network element. If anything fails
         * whilst trying to put the file descriptor, the method
         * will return NULL
         */
        NetworkElement* put (int fd, void* ptr, FiniteStateMachine start);

        /**
         * Get the network element for the given file
         * descriptor.
         * 
         * @param fd the file descriptor for the lookup
         * @return the network element for that given file
         * descriptor, or NULL if the file descriptor isn't
         * inside the queue.
         */
        NetworkElement* get_network_element_from_fd (int fd);

        /**
         * Close the current file descriptor and remove it from
         * the queue.
         */
        void close (int fd);
        /**
         * Close the current network element and remove it from
         * the queue. Fires onClose once, closes the file descriptor,
         * and frees the element after the current epoll batch.
         */
        void close (NetworkElement* element, close_reason reason = CLOSE_ERROR);

        /**
         * Take a network element (that is an object containing
         * the file descriptor, data of the element), and read
         * from it until it outputs EAGAIN.
         * 
         * The element must be owned by the network queue
         * 
         * @param element the network element to be processed
         */
        void process (NetworkElement* element);
        /**
         * Calls epoll_wait to get all the available events
         * and their respective network elements, before
         * calling process on them.
         */
        void wait_and_process ();
        
        /**
         * Put a TUN file descriptor into the queue
         * with a pointer to some data the user wants
         * to store.
         * 
         * The user GIVES OWNERSHIP of the file descriptor
         * to the network queue. One should never close it 
         * manually. If the user wishes to close the file
         * descriptor, it should use the close method of
         * the network queue.
         * 
         * The user still HAS OWNERSHIP over the pointer of
         * the data it wanted to store and will be responsible
         * for freeing it or handling it upon receival of an
         * error or a close through the NetworkQueueHandler.
         * 
         * The user DOES NOT HAVE OWNERSHIP over the resulting
         * network element and should in no case free it, close 
         * the internal file descriptor or manipulate the state,
         * buffer and number of bytes used in the buffer.
         * 
         * The user MAY modify the network's element internal
         * pointer to some new data it wants to use to handle
         * for example a client reconnecting.
         * 
         * @return the generated network element. If anything fails
         * whilst trying to put the file descriptor, the method
         * will return NULL
         */
        NetworkElement* put_tun (int fd, void* ptr);
        /**
         * Put a file descriptor for a socket into the
         * queue with a pointer to some data the user 
         * wants to store.
         * 
         * The user GIVES OWNERSHIP of the file descriptor
         * to the network queue. One should never close it 
         * manually. If the user wishes to close the file
         * descriptor, it should use the close method of
         * the network queue.
         * 
         * The user still HAS OWNERSHIP over the pointer of
         * the data it wanted to store and will be responsible
         * for freeing it or handling it upon receival of an
         * error or a close through the NetworkQueueHandler.
         * 
         * The user DOES NOT HAVE OWNERSHIP over the resulting
         * network element and should in no case free it, close 
         * the internal file descriptor or manipulate the state,
         * buffer and number of bytes used in the buffer.
         * 
         * The user MAY modify the network's element internal
         * pointer to some new data it wants to use to handle
         * for example a client reconnecting.
         * 
         * @return the generated network element. If anything fails
         * whilst trying to put the file descriptor, the method
         * will return NULL
         */
        NetworkElement* put_sck (int fd, void* ptr);

        /**
         * Put a socket whose connect() is in progress (EINPROGRESS) or
         * just completed. When the connection is up, the TLS handshake
         * runs on it if ssl is given (a client session from
         * tls::new_session), and onConnected fires once packets can flow.
         * The queue owns ssl from here on.
         */
        NetworkElement* put_sck_connecting (int fd, void* ptr, ssl_st* ssl);

        /**
         * Put an accepted socket that must complete a TLS handshake as the
         * server before any packet is read. ssl is a fresh session from
         * tls::new_session; the queue owns it. onConnected fires when the
         * handshake is done.
         */
        NetworkElement* put_sck_tls_server (int fd, void* ptr, ssl_st* ssl);

        /**
         * Put a connected socket that must complete a TLS handshake as the
         * client before any packet is read. Same ownership and callback as
         * put_sck_tls_server.
         */
        NetworkElement* put_sck_tls_client (int fd, void* ptr, ssl_st* ssl);

        /**
         * Attach a finished TLS session to a socket already in the queue.
         * Later reads and writes go through SSL_read / SSL_write.
         */
        void adopt_tls(int fd, ssl_st* ssl) {
            NetworkElement* element = get_network_element_from_fd(fd);
            if (!element) return;
            element->ssl = ssl;
        }

        /**
         * Handshake can pull the first application record into OpenSSL.
         * Epoll will not see those bytes, so read them now.
         */
        void drain_tls(int fd);

        /**
         * Create a network queue with the given
         * handler for the events.
         * 
         * @return the network queue created. If anything fails,
         * it will return NULL. 
         */
        NetQueue (NetworkQueueHandler handler);
        /**
         * Create a network queue with the given
         * handler for the events and given wait timeout.
         * 
         * @return the network queue created. If anything fails,
         * it will return NULL. 
         */
        NetQueue (NetworkQueueHandler handler, int epoll_timeout);

        /**
         * Destroy the network queue. This will free all of the
         * network elements and cleanup the EPOLL. This won't
         * handle destruction of the user data.
         */
        ~NetQueue ();
    };
};

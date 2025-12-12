
#pragma once

#include "vnet/netqueue/fsm.hpp"
#include "vnet/netqueue/element.hpp"
#include "vnet/netqueue/handler.hpp"

#include <unordered_map>
#include <mutex>

namespace vnet::netqueue {
    /**
     * Maximum number for events returned
     * by a single call to epoll_wait
     */
    const int EPOLL_MAX_EVENTS = 32;

    /**
     * Instances of this class represents a general
     * purpose pool of file descriptors intended to
     * represent a queue for incoming network packets. 
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

        /*
         * Handler of events (e.g. receiving a packet,
         *   error of a specific fd etc...)
         * 
         * The memory of the handler is owned by this object.
         */
        NetworkQueueHandler handler;

    public:
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
         * the queue. It also closes the file descriptor and frees
         * the network element.
         */
        void close (NetworkElement* element);

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

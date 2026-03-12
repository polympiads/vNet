#pragma once

#include <cstdint>
#include <unordered_map>

namespace vnet::blackbox {

    /**
     * Maintains the next-hop switch for each destination IPv4 address
     * that is not owned by a local agent.
     *
     * Populated by PacketNextForTarget messages from the conductor.
     *
     * When switch_ipv4 is 0 in PacketNextForTarget, the target has
     * disconnected and the route should be removed.
     *
     * Thread safety: NOT thread-safe. Single-threaded event loop.
     */
    class RoutingTable {
    public:
        /**
         * Set or update the next-hop for a destination IPv4.
         *
         * @param dest_ipv4       Target IP in network order.
         * @param next_switch_fd  The fd of the TCP connection to the
         *                        next switch to forward packets to.
         */
        void set_route(uint32_t dest_ipv4, int next_switch_fd);

        /**
         * Remove a route. Called when the conductor sends a
         * PacketNextForTarget with switch_ipv4 = 0 (target disconnected),
         * or when a switch-to-switch connection drops.
         */
        void remove_route(uint32_t dest_ipv4);

        /**
         * Remove all routes that point to a specific switch fd.
         * Called when a switch-to-switch connection drops.
         */
        void remove_routes_for_switch(int switch_fd);

        /**
         * Look up the next-hop fd for a destination IPv4.
         *
         * @return The switch fd, or -1 if no route exists.
         */
        int lookup(uint32_t dest_ipv4) const;

        /** Number of entries in the table. */
        size_t size() const;

        void clear();

    private:
        std::unordered_map<uint32_t, int> routes_;
    };

};

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "vnet/blackbox/config.hpp"
#include "vnet/blackbox/agent_registry.hpp"
#include "vnet/blackbox/routing_table.hpp"
#include "vnet/blackbox/ipv4.hpp"

namespace vnet::blackbox {

    // -----------------------------------------------------------------
    //  Types for the source of an incoming packet
    // -----------------------------------------------------------------

    /**
     * Tells the black box what kind of fd delivered the packet.
     * The switch already knows this from ConnInfo::role.
     */
    enum class SourceType {
        AGENT,      // From a local agent (needs source validation)
        SWITCH,     // From a peer switch (source already validated by origin switch)
        TUN         // From the internet TUN device (return traffic)
    };

    // -----------------------------------------------------------------
    //  Types for the packet decision
    // -----------------------------------------------------------------

    enum class PacketAction {
        DELIVER_LOCAL,      // Write the raw IPv4 payload to target_fd (local agent)
        FORWARD_SWITCH,     // Wrap in PacketIPv4Raw and send to target_fd (peer switch)
        FORWARD_INTERNET,   // Write the raw IPv4 payload to the TUN fd
        REQUEST_ROUTE,      // No route known; caller should send PacketPrepareRouteForTarget
        DROP                // Drop the packet
    };

    enum class DropReason {
        NONE,
        MALFORMED_PACKET,       // IPv4 header parse failed
        SOURCE_SPOOF,           // Source IP doesn't match the agent's registered IP
        SOURCE_UNKNOWN,         // Source fd not found in agent registry
        DEST_UNREACHABLE,       // No local agent, no route, no internet
        ACL_DENIED              // ACL check failed (agent-to-agent or internet)
    };

    struct PacketDecision {
        PacketAction action       = PacketAction::DROP;
        int          target_fd    = -1;         // for DELIVER_LOCAL, FORWARD_SWITCH
        uint32_t     dest_ipv4    = 0;          // for REQUEST_ROUTE
        DropReason   drop_reason  = DropReason::NONE;
    };

    // -----------------------------------------------------------------
    //  BlackBox
    // -----------------------------------------------------------------

    /**
     * The switch's packet processing engine.
     *
     * Responsibilities:
     *   - Source validation (anti-spoofing for agent-sourced packets)
     *   - Destination lookup (local agent, remote agent via routing, or internet)
     *   - Packet decision (deliver, forward, request route, or drop)
     *
     * Does NOT perform I/O. The switch event loop calls process() and
     * acts on the returned PacketDecision.
     *
     * Thread safety: NOT thread-safe. Intended for single-threaded
     * event loop usage.
     *
     * Lifecycle hooks:
     *   - on_agent_authenticated() : called after MIP, populates agent registry
     *   - on_agent_disconnected()  : called on fd close, cleans up
     *   - on_route_update()        : called on PacketNextForTarget from conductor
     *   - on_switch_disconnected() : called when a peer switch drops
     */
    class BlackBox {
    public:
        /**
         * Construct a black box with the given config.
         *
         * @param config  Parsed config (agent→IP mappings, internet permissions).
         *                The BlackBox takes a copy.
         * @param tun_fd  File descriptor of the internet TUN device,
         *                or -1 if internet is not available.
         */
        BlackBox(const Config& config, int tun_fd);

        // ----- Packet processing -----

        /**
         * Process a raw IPv4 packet and decide what to do with it.
         *
         * @param source_type  What kind of fd delivered the packet.
         * @param source_fd    The fd that delivered the packet.
         * @param raw_ipv4     Pointer to the raw IPv4 packet bytes.
         * @param len          Number of bytes in raw_ipv4.
         * @return A PacketDecision telling the caller what to do.
         */
        PacketDecision process(SourceType source_type, int source_fd,
                               const uint8_t* raw_ipv4, size_t len);

        // ----- Lifecycle hooks (called by the switch) -----

        /**
         * An agent has completed MIP and been authenticated.
         * Looks up the config for the agent's virtual IP and
         * registers it in the agent registry.
         *
         * @param name  Agent name (from MIP).
         * @param fd    The agent's TCP fd.
         * @return true if the agent was found in the config and registered.
         */
        bool on_agent_authenticated(const std::string& name, int fd);

        /**
         * An agent's fd was closed (disconnect, error, etc.).
         * Removes the agent from the registry.
         */
        void on_agent_disconnected(int fd);

        /**
         * Received PacketNextForTarget from the conductor.
         * Updates the routing table.
         *
         * @param dest_ipv4       Target IP (network order).
         * @param next_switch_fd  The peer switch fd to forward to,
         *                        or -1 to remove the route (target disconnected).
         */
        void on_route_update(uint32_t dest_ipv4, int next_switch_fd);

        /**
         * A peer switch fd was closed.
         * Removes all routes pointing to that switch.
         */
        void on_switch_disconnected(int switch_fd);

        // ----- Accessors (for logging, debugging) -----

        const AgentRegistry& agents() const;
        const RoutingTable&  routes() const;
        const Config&        config() const;

    private:
        PacketDecision process_from_agent(int source_fd,
                                          const IPv4Header& hdr,
                                          const uint8_t* raw, size_t len);

        PacketDecision process_from_switch(const IPv4Header& hdr,
                                           const uint8_t* raw, size_t len);

        PacketDecision process_from_tun(const IPv4Header& hdr,
                                        const uint8_t* raw, size_t len);

        PacketDecision route_to_destination(uint32_t src_ipv4,
                                            uint32_t dest_ipv4);

        Config         config_;
        AgentRegistry  registry_;
        RoutingTable   routing_;
        int            tun_fd_;
    };

};

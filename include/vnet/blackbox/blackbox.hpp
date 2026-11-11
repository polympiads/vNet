#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

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
        SWITCH,     // From a peer switch (ACL and source route checked again here)
        TUN         // From the internet TUN device (return traffic)
    };

    // -----------------------------------------------------------------
    //  Types for the packet decision
    // -----------------------------------------------------------------

    enum class PacketAction {
        DELIVER_LOCAL,      // Write the raw IPv4 payload to target_fd (local agent)
        FORWARD_SWITCH,     // Send as IPV4_RAW to target_fd (peer switch)
        FORWARD_INTERNET,   // Write the raw IPv4 payload to the TUN fd
        REQUEST_ROUTE,      // Destination is configured but its route is not known yet; hold the packet
        DROP                // Drop the packet
    };

    enum class DropReason {
        NONE,
        MALFORMED_PACKET,       // IPv4 header parse failed
        SOURCE_SPOOF,           // Source IP doesn't match the agent's registered IP
        SOURCE_UNKNOWN,         // Source fd not found in agent registry
        DEST_UNREACHABLE,       // No local agent, no route, no internet
        ACL_DENIED,             // ACL check failed (agent-to-agent or internet)
        RATE_LIMITED,           // The agent sent more than its limit allows
        COUNT
    };

    const char* drop_reason_name(DropReason reason);

    /** What the black box decided since it was made. */
    struct Counters {
        uint64_t delivered_local    = 0;
        uint64_t forwarded_switch   = 0;
        uint64_t forwarded_internet = 0;
        uint64_t route_requested    = 0;
        uint64_t dropped[static_cast<size_t>(DropReason::COUNT)] = {0};
        uint64_t dropped_total() const;
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
     *   - on_route_update()        : called on SWITCH_ROUTE_UPDATE and AGENT_UNREGISTERED from the conductor
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

        /**
         * Limit what each agent may send: packets per second and bytes
         * per second, each with a burst of one second's worth. 0 means
         * no limit. Applies to agents registered after the call.
         */
        void set_agent_limits(uint64_t pps, uint64_t bytes_per_sec);

        /**
         * Put a newly loaded config in force. Rules apply to the next
         * packet. Agents already registered keep the virtual IP they
         * were given; a changed or removed assignment takes effect when
         * that agent next connects.
         */
        void replace_config(const Config& config);

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
         * The conductor published or withdrew the route to an agent on
         * another switch. Updates the routing table.
         *
         * @param dest_ipv4       Target IP (network order).
         * @param next_switch_fd  The peer switch fd to forward to,
         *                        or -1 to remove the route (target disconnected).
         */
        void on_route_update(uint32_t dest_ipv4, int next_switch_fd);

        /**
         * A peer switch fd was closed.
         * Removes all routes pointing to that switch.
         *
         * @return The destinations that were routed through it.
         */
        std::vector<uint32_t> on_switch_disconnected(int switch_fd);

        /**
         * A peer session moved from old_fd to new_fd.
         * Routes stay; only the socket changes.
         */
        void retarget_switch(int old_fd, int new_fd);

        // ----- Accessors (for logging, debugging) -----

        const AgentRegistry& agents() const;
        const RoutingTable&  routes() const;
        const Config&        config() const;
        const Counters&      counters() const;

        /** The counters as one line, and one line per agent. */
        std::string summary() const;
        std::string agent_lines() const;

    private:
        PacketDecision process_from_agent(int source_fd,
                                          const IPv4Header& hdr,
                                          const uint8_t* raw, size_t len);

        PacketDecision process_from_switch(int source_fd,
                                           const IPv4Header& hdr,
                                           const uint8_t* raw, size_t len);

        PacketDecision process_from_tun(const IPv4Header& hdr,
                                        const uint8_t* raw, size_t len);

        PacketDecision route_to_destination(uint32_t src_ipv4,
                                            uint32_t dest_ipv4);
        PacketDecision record(const PacketDecision& decision, size_t len);

        Config         config_;
        AgentRegistry  registry_;
        RoutingTable   routing_;
        int            tun_fd_;
        uint64_t       limit_pps_ = 0;
        uint64_t       limit_bps_ = 0;
        Counters       counters_;
    };

};

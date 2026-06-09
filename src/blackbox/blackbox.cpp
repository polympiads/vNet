#include "vnet/blackbox/blackbox.hpp"

#include <iostream>

namespace vnet::blackbox {

    BlackBox::BlackBox(const Config& config, int tun_fd)
        : config_(config), tun_fd_(tun_fd) {}

    // =================================================================
    //  Lifecycle hooks
    // =================================================================

    bool BlackBox::on_agent_authenticated(const std::string& name, int fd) {
        uint32_t ipv4 = config_.find_ip_for_agent(name);
        if (ipv4 == 0) {
            std::cerr << "[BlackBox] Agent '" << name
                      << "' not found in config, rejecting\n";
            return false;
        }

        AgentEntry* entry = registry_.register_agent(name, ipv4, fd);
        if (!entry) {
            std::cerr << "[BlackBox] Failed to register agent '" << name << "'\n";
            return false;
        }

        std::cout << "[BlackBox] Agent registered: " << name
                  << " (fd=" << fd << ")\n";

        return true;
    }

    void BlackBox::on_agent_disconnected(int fd) {
        AgentEntry* entry = registry_.find_by_fd(fd);
        if (entry) {
            std::cout << "[BlackBox] Agent disconnected: " << entry->name
                      << " (fd=" << fd << ")\n";
        }
        registry_.unregister_by_fd(fd);
    }

    void BlackBox::on_route_update(uint32_t dest_ipv4, int next_switch_fd) {
        if (next_switch_fd < 0) {
            routing_.remove_route(dest_ipv4);
        } else {
            routing_.set_route(dest_ipv4, next_switch_fd);
        }
    }

    void BlackBox::on_switch_disconnected(int switch_fd) {
        routing_.remove_routes_for_switch(switch_fd);
    }

    // =================================================================
    //  Accessors
    // =================================================================

    const AgentRegistry& BlackBox::agents() const { return registry_; }
    const RoutingTable&  BlackBox::routes() const  { return routing_; }
    const Config&        BlackBox::config() const  { return config_; }

    // =================================================================
    //  Packet processing — entry point
    // =================================================================

    PacketDecision BlackBox::process(SourceType source_type, int source_fd,
                                     const uint8_t* raw_ipv4, size_t len) {
        IPv4Header hdr{};
        if (!ipv4_parse(raw_ipv4, len, hdr)) {
            return { PacketAction::DROP, -1, 0, DropReason::MALFORMED_PACKET };
        }

        switch (source_type) {
            case SourceType::AGENT:
                return process_from_agent(source_fd, hdr, raw_ipv4, len);
            case SourceType::SWITCH:
                return process_from_switch(hdr, raw_ipv4, len);
            case SourceType::TUN:
                return process_from_tun(hdr, raw_ipv4, len);
        }

        return { PacketAction::DROP, -1, 0, DropReason::MALFORMED_PACKET };
    }

    // =================================================================
    //  Packet from a local agent
    //
    //  1. Validate source IP (anti-spoofing)
    //  2. Check ACL + route to destination
    // =================================================================

    PacketDecision BlackBox::process_from_agent(int source_fd,
                                                const IPv4Header& hdr,
                                                const uint8_t* raw,
                                                size_t len) {
        // Step 1: Find the agent that owns this fd
        AgentEntry* sender = registry_.find_by_fd(source_fd);
        if (!sender) {
            return { PacketAction::DROP, -1, 0, DropReason::SOURCE_UNKNOWN };
        }

        // Step 2: Anti-spoofing — the source IP in the packet MUST
        //         match the virtual IP assigned to this agent.
        if (hdr.src_ip != sender->virtual_ipv4) {
            return { PacketAction::DROP, -1, 0, DropReason::SOURCE_SPOOF };
        }

        // Step 3: ACL + route to destination
        return route_to_destination(sender->virtual_ipv4, hdr.dst_ip);
    }

    // =================================================================
    //  Packet from a peer switch
    //
    //  The source switch already checked outgoing ACLs.
    //  Since ACL rules are bidirectional, if the source switch
    //  allowed it, the incoming side is also authorized.
    //  We just deliver to the local agent.
    // =================================================================

    PacketDecision BlackBox::process_from_switch(const IPv4Header& hdr,
                                                  const uint8_t* raw,
                                                  size_t len) {
        AgentEntry* dest = registry_.find_by_ipv4(hdr.dst_ip);
        if (dest) {
            return { PacketAction::DELIVER_LOCAL, dest->fd, 0, DropReason::NONE };
        }

        return { PacketAction::DROP, -1, 0, DropReason::DEST_UNREACHABLE };
    }

    // =================================================================
    //  Packet from the internet TUN (return traffic)
    //
    //  The original outgoing packet was ACL-checked.
    //  For return traffic, verify that the destination agent is
    //  allowed to reach the source internet IP (bidirectional rule).
    // =================================================================

    PacketDecision BlackBox::process_from_tun(const IPv4Header& hdr,
                                               const uint8_t* raw,
                                               size_t len) {
        AgentEntry* dest = registry_.find_by_ipv4(hdr.dst_ip);
        if (!dest) {
            return { PacketAction::DROP, -1, 0, DropReason::DEST_UNREACHABLE };
        }

        // Verify that this agent has an ACL rule allowing traffic
        // to/from this internet IP.
        if (!config_.can_reach_internet_ip(hdr.dst_ip, hdr.src_ip)) {
            return { PacketAction::DROP, -1, 0, DropReason::ACL_DENIED };
        }

        return { PacketAction::DELIVER_LOCAL, dest->fd, 0, DropReason::NONE };
    }

    // =================================================================
    //  Destination routing with ACL enforcement
    //
    //  Priority:
    //    1. Known agent (local or remote) → check ACL → deliver/forward
    //    2. Unknown IP → internet → check internet ACL → forward to TUN
    //    3. Drop
    // =================================================================

    PacketDecision BlackBox::route_to_destination(uint32_t src_ipv4,
                                                   uint32_t dest_ipv4) {
        // 1. Is the destination a known agent?
        std::string dest_agent = config_.find_agent_for_ip(dest_ipv4);
        if (!dest_agent.empty()) {
            // ACL check: can source talk to destination?
            if (!config_.can_agents_communicate(src_ipv4, dest_ipv4)) {
                return { PacketAction::DROP, -1, 0, DropReason::ACL_DENIED };
            }

            // Local agent?
            AgentEntry* local = registry_.find_by_ipv4(dest_ipv4);
            if (local) {
                return { PacketAction::DELIVER_LOCAL, local->fd, 0, DropReason::NONE };
            }

            // Remote agent — check routing table
            int next_switch = routing_.lookup(dest_ipv4);
            if (next_switch >= 0) {
                return { PacketAction::FORWARD_SWITCH, next_switch, 0, DropReason::NONE };
            }

            // No route yet — ask the conductor
            return { PacketAction::REQUEST_ROUTE, -1, dest_ipv4, DropReason::NONE };
        }

        // 2. Unknown IP — this is internet traffic
        if (tun_fd_ < 0) {
            return { PacketAction::DROP, -1, 0, DropReason::DEST_UNREACHABLE };
        }

        // ACL check: can this agent reach this specific internet IP?
        if (!config_.can_reach_internet_ip(src_ipv4, dest_ipv4)) {
            return { PacketAction::DROP, -1, 0, DropReason::ACL_DENIED };
        }

        return { PacketAction::FORWARD_INTERNET, tun_fd_, 0, DropReason::NONE };
    }

};

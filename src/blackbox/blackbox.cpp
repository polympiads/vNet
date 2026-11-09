#include "vnet/blackbox/blackbox.hpp"

#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>
#include <arpa/inet.h>

namespace vnet::blackbox {

    static uint64_t now_ns() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    const char* drop_reason_name(DropReason reason) {
        switch (reason) {
            case DropReason::NONE:             return "none";
            case DropReason::MALFORMED_PACKET: return "malformed";
            case DropReason::SOURCE_SPOOF:     return "spoof";
            case DropReason::SOURCE_UNKNOWN:   return "unknown-source";
            case DropReason::DEST_UNREACHABLE: return "unreachable";
            case DropReason::ACL_DENIED:       return "acl";
            case DropReason::RATE_LIMITED:     return "rate-limited";
            default:                           return "?";
        }
    }

    uint64_t Counters::dropped_total() const {
        uint64_t total = 0;
        for (uint64_t d : dropped) total += d;
        return total;
    }

    BlackBox::BlackBox(const Config& config, int tun_fd)
        : config_(config), tun_fd_(tun_fd) {}

    void BlackBox::set_agent_limits(uint64_t pps, uint64_t bytes_per_sec) {
        limit_pps_ = pps;
        limit_bps_ = bytes_per_sec;
    }

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
        entry->pps = TokenBucket(limit_pps_, limit_pps_);
        entry->bps = TokenBucket(limit_bps_, limit_bps_);

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

    void BlackBox::retarget_switch(int old_fd, int new_fd) {
        routing_.retarget(old_fd, new_fd);
    }

    // =================================================================
    //  Accessors
    // =================================================================

    const AgentRegistry& BlackBox::agents() const   { return registry_; }
    const RoutingTable&  BlackBox::routes() const   { return routing_; }
    const Config&        BlackBox::config() const   { return config_; }
    const Counters&      BlackBox::counters() const { return counters_; }

    std::string BlackBox::summary() const {
        std::ostringstream out;
        out << "agents=" << registry_.size()
            << " routes=" << routing_.size()
            << " local=" << counters_.delivered_local
            << " to-switch=" << counters_.forwarded_switch
            << " to-internet=" << counters_.forwarded_internet
            << " waited-for-route=" << counters_.route_requested
            << " dropped=" << counters_.dropped_total();
        for (size_t i = 1; i < static_cast<size_t>(DropReason::COUNT); i++) {
            if (counters_.dropped[i] == 0) continue;
            out << " " << drop_reason_name(static_cast<DropReason>(i))
                << "=" << counters_.dropped[i];
        }
        return out.str();
    }

    std::string BlackBox::agent_lines() const {
        std::ostringstream out;
        for (const AgentEntry* a : registry_.entries()) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &a->virtual_ipv4, ip, sizeof(ip));
            out << a->name << " " << ip << " fd=" << a->fd
                << " in=" << a->pkts_in << "/" << a->bytes_in << "B"
                << " out=" << a->pkts_out << "/" << a->bytes_out << "B"
                << " dropped=" << a->dropped << "\n";
        }
        return out.str();
    }

    PacketDecision BlackBox::record(const PacketDecision& d, size_t len) {
        switch (d.action) {
            case PacketAction::DELIVER_LOCAL: {
                counters_.delivered_local++;
                AgentEntry* dest = registry_.find_by_fd(d.target_fd);
                if (dest) { dest->pkts_out++; dest->bytes_out += len; }
                break;
            }
            case PacketAction::FORWARD_SWITCH:   counters_.forwarded_switch++;   break;
            case PacketAction::FORWARD_INTERNET: counters_.forwarded_internet++; break;
            case PacketAction::REQUEST_ROUTE:    counters_.route_requested++;    break;
            case PacketAction::DROP:
                counters_.dropped[static_cast<size_t>(d.drop_reason)]++;
                break;
        }
        return d;
    }

    // =================================================================
    //  Packet processing — entry point
    // =================================================================

    PacketDecision BlackBox::process(SourceType source_type, int source_fd,
                                     const uint8_t* raw_ipv4, size_t len) {
        IPv4Header hdr{};
        if (!ipv4_parse(raw_ipv4, len, hdr)) {
            return record({ PacketAction::DROP, -1, 0, DropReason::MALFORMED_PACKET }, len);
        }

        switch (source_type) {
            case SourceType::AGENT:
                return record(process_from_agent(source_fd, hdr, raw_ipv4, len), len);
            case SourceType::SWITCH:
                return record(process_from_switch(source_fd, hdr, raw_ipv4, len), len);
            case SourceType::TUN:
                return record(process_from_tun(hdr, raw_ipv4, len), len);
        }

        return record({ PacketAction::DROP, -1, 0, DropReason::MALFORMED_PACKET }, len);
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

        sender->pkts_in++;
        sender->bytes_in += len;

        // Step 2: Anti-spoofing: the source IP in the packet MUST
        //         match the virtual IP assigned to this agent.
        if (hdr.src_ip != sender->virtual_ipv4) {
            sender->dropped++;
            return { PacketAction::DROP, -1, 0, DropReason::SOURCE_SPOOF };
        }

        // Step 3: the agent's share of the switch. One flooding agent
        //         must not slow every other machine on this switch.
        if (!sender->pps.unlimited() || !sender->bps.unlimited()) {
            uint64_t now = now_ns();
            if (!sender->pps.take(1, now) || !sender->bps.take(len, now)) {
                sender->dropped++;
                return { PacketAction::DROP, -1, 0, DropReason::RATE_LIMITED };
            }
        }

        // Step 4: ACL + route to destination
        PacketDecision d = route_to_destination(sender->virtual_ipv4, hdr.dst_ip);
        if (d.action == PacketAction::DROP) sender->dropped++;
        return d;
    }

    // =================================================================
    //  Packet from a peer switch
    //
    //  The sending switch checked the ACL and the source address, and
    //  every switch holds the same config, so the checks are repeated
    //  here: a peer that is wrong or compromised still cannot reach an
    //  agent the ACL does not allow, nor speak for an agent it does not
    //  hold. A switch never forwards peer traffic on, so the destination
    //  must be local.
    // =================================================================

    PacketDecision BlackBox::process_from_switch(int source_fd,
                                                  const IPv4Header& hdr,
                                                  const uint8_t* raw,
                                                  size_t len) {
        AgentEntry* dest = registry_.find_by_ipv4(hdr.dst_ip);
        if (!dest) {
            return { PacketAction::DROP, -1, 0, DropReason::DEST_UNREACHABLE };
        }

        if (config_.find_agent_for_ip(hdr.src_ip).empty()) {
            return { PacketAction::DROP, -1, 0, DropReason::SOURCE_UNKNOWN };
        }
        // The route for the source, once learned, must point at the peer
        // that sent this. Before it is learned the ACL alone decides.
        int expected = routing_.lookup(hdr.src_ip);
        if (expected >= 0 && expected != source_fd) {
            return { PacketAction::DROP, -1, 0, DropReason::SOURCE_SPOOF };
        }
        if (!config_.can_agents_communicate(hdr.src_ip, hdr.dst_ip)) {
            return { PacketAction::DROP, -1, 0, DropReason::ACL_DENIED };
        }

        return { PacketAction::DELIVER_LOCAL, dest->fd, 0, DropReason::NONE };
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

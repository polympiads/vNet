#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vnet::blackbox {

    /**
     * Parsed configuration for the black box.
     *
     * The config file has one entry per line.
     * Blank lines and lines starting with '#' are ignored.
     *
     * Two kinds of entries:
     *
     *   IP ASSIGNMENT:
     *     agent_name  ip_address
     *
     *     Maps a virtual IPv4 address to an agent name.
     *     Distinguished from ACL rules because the second
     *     token is a valid IPv4 address.
     *
     *   ACL RULE:
     *     agent_name  agent_name
     *       Allows bidirectional communication between two agents.
     *
     *     agent_name  ::internet:ip_address
     *       Allows this agent to reach a specific internet IP.
     *       Return traffic from that IP is also allowed.
     *
     * Example:
     *   # IP assignments
     *   contestant1  10.0.1.1
     *   contestant2  10.0.1.2
     *   gameserver   10.0.0.1
     *
     *   # Contestants can talk to game server only
     *   contestant1  gameserver
     *   contestant2  gameserver
     *
     *   # Game server can reach specific internet hosts
     *   gameserver   ::internet:8.8.8.8
     *   gameserver   ::internet:1.1.1.1
     */
    class Config {
    public:
        /**
         * Load and parse a config file.
         *
         * IP assignments must appear before ACL rules that reference
         * agent names (so that agent names can be validated).
         *
         * @param path  Path to the config file.
         * @return true on success, false on parse error or I/O failure.
         */
        bool load(const std::string& path);

        // ----- IP assignment lookups -----

        /** Find the agent name that owns a given IPv4. Empty if not found. */
        std::string find_agent_for_ip(uint32_t ipv4) const;

        /** Find the virtual IPv4 assigned to an agent. 0 if not found. */
        uint32_t find_ip_for_agent(const std::string& name) const;

        /** All configured agent names and their IPs. */
        const std::unordered_map<std::string, uint32_t>& agents() const;

        // ----- ACL lookups -----

        /**
         * Can the agent at src_ipv4 communicate with the agent at dst_ipv4?
         * Bidirectional: if A→B is allowed, B→A is also allowed.
         */
        bool can_agents_communicate(uint32_t src_ipv4, uint32_t dst_ipv4) const;

        /**
         * Can the agent at agent_ipv4 reach the given internet IP?
         * Also used for return traffic: if agent can reach internet_ipv4,
         * then return traffic from internet_ipv4 to agent is allowed.
         */
        bool can_reach_internet_ip(uint32_t agent_ipv4, uint32_t internet_ipv4) const;

        /**
         * Does this agent have any internet rules at all?
         */
        bool has_any_internet_access(uint32_t agent_ipv4) const;

    private:
        // IP assignments
        std::unordered_map<std::string, uint32_t> name_to_ip_;
        std::unordered_map<uint32_t, std::string> ip_to_name_;

        // ACL: agent-to-agent (bidirectional)
        // agent_ipv4 → set of agent_ipv4s it can talk to
        std::unordered_map<uint32_t, std::unordered_set<uint32_t>> agent_acl_;

        // ACL: agent-to-internet
        // agent_ipv4 → set of internet IPs it can reach
        std::unordered_map<uint32_t, std::unordered_set<uint32_t>> internet_acl_;

        static bool is_ipv4(const std::string& s);
    };

};

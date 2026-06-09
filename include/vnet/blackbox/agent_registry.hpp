#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace vnet::blackbox {

    /**
     * Represents a single agent connected to this switch.
     */
    struct AgentEntry {
        std::string name;
        uint32_t    virtual_ipv4;   // network order
        int         fd;
    };

    /**
     * Maintains the set of agents currently connected to this switch,
     * indexed three ways for O(1) lookup in the packet processing path.
     *
     * Thread safety: NOT thread-safe. Intended to be used from a
     * single-threaded event loop (epoll).
     *
     * Ownership: the registry owns the AgentEntry objects. They are
     * freed when unregistered or when the registry is destroyed.
     */
    class AgentRegistry {
    public:
        /**
         * Register a new agent. If an agent with the same name was
         * already registered, the old entry is removed first.
         *
         * @param name            The agent name (from MIP).
         * @param virtual_ipv4    The agent's virtual IP (network order, from config).
         * @param fd              The TCP fd for communicating with the agent.
         * @return pointer to the created entry, or nullptr on failure.
         */
        AgentEntry* register_agent(const std::string& name,
                                   uint32_t virtual_ipv4,
                                   int fd);

        /**
         * Remove an agent by fd. Called when the agent disconnects.
         * Frees the AgentEntry.
         */
        void unregister_by_fd(int fd);

        /**
         * Remove an agent by name. Frees the AgentEntry.
         */
        void unregister_by_name(const std::string& name);

        /** Look up an agent by its virtual IPv4. O(1). */
        AgentEntry* find_by_ipv4(uint32_t ipv4) const;

        /** Look up an agent by its TCP fd. O(1). */
        AgentEntry* find_by_fd(int fd) const;

        /** Look up an agent by name. O(1). */
        AgentEntry* find_by_name(const std::string& name) const;

        /** Number of registered agents. */
        size_t size() const;

        ~AgentRegistry();

    private:
        void remove_entry(AgentEntry* entry);

        std::unordered_map<uint32_t, AgentEntry*>     by_ipv4_;
        std::unordered_map<int, AgentEntry*>           by_fd_;
        std::unordered_map<std::string, AgentEntry*>   by_name_;
    };

};

#include "vnet/blackbox/agent_registry.hpp"

namespace vnet::blackbox {

    AgentEntry* AgentRegistry::register_agent(const std::string& name,
                                              uint32_t virtual_ipv4,
                                              int fd) {
        // If this name is already registered, clean up the old entry
        // (handles agent reconnection to the same switch).
        auto it = by_name_.find(name);
        if (it != by_name_.end()) {
            remove_entry(it->second);
        }

        auto* entry = new (std::nothrow) AgentEntry();
        if (!entry) return nullptr;

        entry->name         = name;
        entry->virtual_ipv4 = virtual_ipv4;
        entry->fd           = fd;

        by_ipv4_[virtual_ipv4] = entry;
        by_fd_[fd]             = entry;
        by_name_[name]         = entry;

        return entry;
    }

    void AgentRegistry::unregister_by_fd(int fd) {
        auto it = by_fd_.find(fd);
        if (it == by_fd_.end()) return;
        remove_entry(it->second);
    }

    void AgentRegistry::unregister_by_name(const std::string& name) {
        auto it = by_name_.find(name);
        if (it == by_name_.end()) return;
        remove_entry(it->second);
    }

    AgentEntry* AgentRegistry::find_by_ipv4(uint32_t ipv4) const {
        auto it = by_ipv4_.find(ipv4);
        return (it != by_ipv4_.end()) ? it->second : nullptr;
    }

    AgentEntry* AgentRegistry::find_by_fd(int fd) const {
        auto it = by_fd_.find(fd);
        return (it != by_fd_.end()) ? it->second : nullptr;
    }

    AgentEntry* AgentRegistry::find_by_name(const std::string& name) const {
        auto it = by_name_.find(name);
        return (it != by_name_.end()) ? it->second : nullptr;
    }

    size_t AgentRegistry::size() const {
        return by_name_.size();
    }

    void AgentRegistry::remove_entry(AgentEntry* entry) {
        if (!entry) return;

        by_ipv4_.erase(entry->virtual_ipv4);
        by_fd_.erase(entry->fd);
        by_name_.erase(entry->name);

        delete entry;
    }

    AgentRegistry::~AgentRegistry() {
        // Delete all remaining entries.
        // Iterate by_name_ since each entry appears exactly once there.
        for (auto& [name, entry] : by_name_) {
            delete entry;
        }
        by_ipv4_.clear();
        by_fd_.clear();
        by_name_.clear();
    }

};

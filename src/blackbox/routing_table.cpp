#include "vnet/blackbox/routing_table.hpp"

#include <algorithm>

namespace vnet::blackbox {

    void RoutingTable::set_route(uint32_t dest_ipv4, int next_switch_fd) {
        routes_[dest_ipv4] = next_switch_fd;
    }

    void RoutingTable::remove_route(uint32_t dest_ipv4) {
        routes_.erase(dest_ipv4);
    }

    void RoutingTable::remove_routes_for_switch(int switch_fd) {
        for (auto it = routes_.begin(); it != routes_.end(); ) {
            if (it->second == switch_fd)
                it = routes_.erase(it);
            else
                ++it;
        }
    }

    int RoutingTable::lookup(uint32_t dest_ipv4) const {
        auto it = routes_.find(dest_ipv4);
        return (it != routes_.end()) ? it->second : -1;
    }

    size_t RoutingTable::size() const {
        return routes_.size();
    }

    void RoutingTable::clear() {
        routes_.clear();
    }

};

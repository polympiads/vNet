#include "vnet/blackbox/config.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <arpa/inet.h>

namespace vnet::blackbox {

    static uint32_t parse_ipv4(const std::string& s) {
        uint32_t addr = 0;
        if (inet_pton(AF_INET, s.c_str(), &addr) != 1)
            return 0;
        return addr;    // network order
    }

    bool Config::is_ipv4(const std::string& s) {
        uint32_t dummy = 0;
        return inet_pton(AF_INET, s.c_str(), &dummy) == 1;
    }

    static const std::string INTERNET_PREFIX = "::internet:";

    bool Config::load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "[Config] Cannot open " << path << "\n";
            return false;
        }

        name_to_ip_.clear();
        ip_to_name_.clear();
        agent_acl_.clear();
        internet_acl_.clear();

        // We do two passes:
        //  Pass 1: collect all lines
        //  Pass 2: process IP assignments first, then ACL rules
        // This way agent names in ACL rules can be validated.

        struct Line {
            int         num;
            std::string first;
            std::string second;
        };

        std::vector<Line> lines;
        std::string raw;
        int line_num = 0;

        while (std::getline(file, raw)) {
            line_num++;

            size_t start = raw.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            if (raw[start] == '#') continue;

            std::istringstream iss(raw.substr(start));
            std::string first, second;
            iss >> first >> second;

            if (first.empty() || second.empty()) {
                std::cerr << "[Config] Line " << line_num
                          << ": expected two tokens, got '" << raw << "'\n";
                return false;
            }

            lines.push_back({line_num, first, second});
        }

        // --- Pass 1: IP assignments (second token is a valid IPv4) ---
        for (auto& l : lines) {
            if (!is_ipv4(l.second))
                continue;

            uint32_t ipv4 = parse_ipv4(l.second);
            if (ipv4 == 0) {
                std::cerr << "[Config] Line " << l.num
                          << ": invalid IPv4 '" << l.second << "'\n";
                return false;
            }

            if (ip_to_name_.count(ipv4)) {
                std::cerr << "[Config] Line " << l.num
                          << ": duplicate IP " << l.second << "\n";
                return false;
            }
            if (name_to_ip_.count(l.first)) {
                std::cerr << "[Config] Line " << l.num
                          << ": duplicate agent name '" << l.first << "'\n";
                return false;
            }

            name_to_ip_[l.first] = ipv4;
            ip_to_name_[ipv4]    = l.first;
        }

        // --- Pass 2: ACL rules (second token is agent name or ::internet:ip) ---
        for (auto& l : lines) {
            if (is_ipv4(l.second))
                continue;   // already processed

            // Validate the source agent exists
            auto src_it = name_to_ip_.find(l.first);
            if (src_it == name_to_ip_.end()) {
                std::cerr << "[Config] Line " << l.num
                          << ": unknown agent '" << l.first << "'\n";
                return false;
            }
            uint32_t src_ip = src_it->second;

            if (l.second.rfind(INTERNET_PREFIX, 0) == 0) {
                // ::internet:ip_address
                std::string ip_str = l.second.substr(INTERNET_PREFIX.size());
                uint32_t inet_ip = parse_ipv4(ip_str);
                if (inet_ip == 0) {
                    std::cerr << "[Config] Line " << l.num
                              << ": invalid internet IP in '" << l.second << "'\n";
                    return false;
                }

                internet_acl_[src_ip].insert(inet_ip);
            } else {
                // agent-to-agent rule
                auto dst_it = name_to_ip_.find(l.second);
                if (dst_it == name_to_ip_.end()) {
                    std::cerr << "[Config] Line " << l.num
                              << ": unknown agent '" << l.second << "'\n";
                    return false;
                }
                uint32_t dst_ip = dst_it->second;

                // Bidirectional
                agent_acl_[src_ip].insert(dst_ip);
                agent_acl_[dst_ip].insert(src_ip);
            }
        }

        std::cout << "[Config] Loaded " << name_to_ip_.size() << " agent(s), "
                  << agent_acl_.size() << " with agent ACLs, "
                  << internet_acl_.size() << " with internet ACLs\n";

        return true;
    }

    // ----- IP assignment lookups -----

    std::string Config::find_agent_for_ip(uint32_t ipv4) const {
        auto it = ip_to_name_.find(ipv4);
        return (it != ip_to_name_.end()) ? it->second : "";
    }

    uint32_t Config::find_ip_for_agent(const std::string& name) const {
        auto it = name_to_ip_.find(name);
        return (it != name_to_ip_.end()) ? it->second : 0;
    }

    const std::unordered_map<std::string, uint32_t>& Config::agents() const {
        return name_to_ip_;
    }

    // ----- ACL lookups -----

    bool Config::can_agents_communicate(uint32_t src_ipv4, uint32_t dst_ipv4) const {
        auto it = agent_acl_.find(src_ipv4);
        if (it == agent_acl_.end()) return false;
        return it->second.count(dst_ipv4) > 0;
    }

    bool Config::can_reach_internet_ip(uint32_t agent_ipv4, uint32_t internet_ipv4) const {
        auto it = internet_acl_.find(agent_ipv4);
        if (it == internet_acl_.end()) return false;
        return it->second.count(internet_ipv4) > 0;
    }

    bool Config::has_any_internet_access(uint32_t agent_ipv4) const {
        auto it = internet_acl_.find(agent_ipv4);
        return it != internet_acl_.end() && !it->second.empty();
    }

};

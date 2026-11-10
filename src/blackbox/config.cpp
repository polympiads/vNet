#include "vnet/blackbox/config.hpp"
#include "common/sysutil.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <arpa/inet.h>
#include <openssl/evp.h>

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
        Config fresh;
        if (!fresh.parse_into(file, path)) return false;
        *this = fresh;
        return true;
    }

    bool Config::load_text(const std::string& text) {
        std::istringstream in(text);
        Config fresh;
        if (!fresh.parse_into(in, "received config")) return false;
        *this = fresh;
        return true;
    }

    bool Config::parse_into(std::istream& file, const std::string& origin) {
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
            if (!raw.empty() && raw.back() == '\r')
                raw.pop_back();

            size_t start = raw.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            if (raw[start] == '#') continue;

            std::istringstream iss(raw.substr(start));
            std::string first, second;
            iss >> first >> second;

            if (first.empty() || second.empty()) {
                std::cerr << "[Config] " << origin << " line " << line_num
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
                std::cerr << "[Config] " << origin << " line " << l.num
                          << ": invalid IPv4 '" << l.second << "'\n";
                return false;
            }

            if (!valid_name(l.first, AGENT_NAME_MAX)) {
                std::cerr << "[Config] " << origin << " line " << l.num << ": agent name '" << l.first
                          << "' must be 1 to " << AGENT_NAME_MAX
                          << " characters from [A-Za-z0-9_-]\n";
                return false;
            }
            if (ip_to_name_.count(ipv4)) {
                std::cerr << "[Config] " << origin << " line " << l.num
                          << ": duplicate IP " << l.second << "\n";
                return false;
            }
            if (name_to_ip_.count(l.first)) {
                std::cerr << "[Config] " << origin << " line " << l.num
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
                std::cerr << "[Config] " << origin << " line " << l.num
                          << ": unknown agent '" << l.first << "'\n";
                return false;
            }
            uint32_t src_ip = src_it->second;

            if (l.second.rfind(INTERNET_PREFIX, 0) == 0) {
                // ::internet:ip_address or ::internet:a.b.c.d/n
                std::string ip_str = l.second.substr(INTERNET_PREFIX.size());
                uint32_t inet_ip = 0;
                int prefix_len = 32;
                if (!parse_cidr(ip_str, inet_ip, prefix_len) ||
                    (inet_ip == 0 && prefix_len == 32)) {
                    std::cerr << "[Config] " << origin << " line " << l.num
                              << ": invalid internet address in '" << l.second << "'\n";
                    return false;
                }
                uint32_t mask = prefix_len == 0 ? 0 : htonl(~((1ULL << (32 - prefix_len)) - 1));
                internet_acl_[src_ip].push_back({inet_ip & mask, mask, prefix_len});
            } else {
                // agent-to-agent rule
                auto dst_it = name_to_ip_.find(l.second);
                if (dst_it == name_to_ip_.end()) {
                    std::cerr << "[Config] " << origin << " line " << l.num
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
        for (const Prefix& p : it->second)
            if ((internet_ipv4 & p.mask) == p.addr) return true;
        return false;
    }

    std::string Config::fingerprint() const {
        // Canonical text: sorted assignments, sorted pairs each written
        // once with the lower address first, sorted internet prefixes.
        std::vector<std::string> lines;
        for (const auto& [name, ip] : name_to_ip_) {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &ip, buf, sizeof(buf));
            lines.push_back("ip " + name + " " + buf);
        }
        for (const auto& [src, dsts] : agent_acl_) {
            for (uint32_t dst : dsts) {
                uint32_t a = ntohl(src), b = ntohl(dst);
                if (a > b) continue;
                lines.push_back("pair " + std::to_string(a) + " " + std::to_string(b));
            }
        }
        for (const auto& [src, prefixes] : internet_acl_) {
            for (const Prefix& p : prefixes)
                lines.push_back("inet " + std::to_string(ntohl(src)) + " " +
                                std::to_string(ntohl(p.addr)) + "/" + std::to_string(p.length));
        }
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        std::string text;
        for (const auto& l : lines) text += l + "\n";

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  digest_len = 0;
        if (!EVP_Digest(text.data(), text.size(), digest, &digest_len, EVP_sha256(), nullptr))
            return "";
        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < digest_len; i++)
            hex << std::setw(2) << static_cast<int>(digest[i]);
        return hex.str();
    }

    bool Config::has_any_internet_access(uint32_t agent_ipv4) const {
        auto it = internet_acl_.find(agent_ipv4);
        return it != internet_acl_.end() && !it->second.empty();
    }

};

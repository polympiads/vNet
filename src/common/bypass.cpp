#include "common/bypass.h"
#include "common/sysutil.h"

#include <fstream>
#include <iostream>
#include <sstream>

#include <arpa/inet.h>

static uint32_t mask_of(int prefix) {
    if (prefix == 0) return 0;
    return htonl(0xFFFFFFFFu << (32 - prefix));
}

/* Two prefixes overlap when the shorter one contains the other's address. */
static bool overlaps(uint32_t a, int pa, uint32_t b, int pb) {
    int shorter = pa < pb ? pa : pb;
    uint32_t m = mask_of(shorter);
    return (a & m) == (b & m);
}

static std::string cidr_text(uint32_t addr, int prefix) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return std::string(buf) + "/" + std::to_string(prefix);
}

bool parse_bypass(const std::string& text, const std::string& overlay_cidr,
                  std::vector<BypassRule>& rules, const char* tag) {
    uint32_t overlay_addr = 0;
    int overlay_prefix = 0;
    if (!parse_cidr(overlay_cidr, overlay_addr, overlay_prefix)) {
        std::cerr << tag << " Overlay prefix is not a.b.c.d/n: " << overlay_cidr << "\n";
        return false;
    }

    std::vector<BypassRule> fresh;
    std::istringstream in(text);
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        line_no++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;

        std::istringstream words(line.substr(start));
        std::string kind;
        words >> kind;
        std::vector<std::string> args;
        for (std::string w; words >> w; ) args.push_back(w);

        BypassRule rule;
        if (kind == "route") {
            rule.kind = BypassRule::ROUTE;
            if (args.size() != 1 || !parse_cidr(args[0], rule.addr, rule.prefix)) {
                std::cerr << tag << " Bypass file line " << line_no
                          << ": expected 'route a.b.c.d/n'\n";
                return false;
            }
            if (rule.prefix == 0) {
                std::cerr << tag << " Bypass file line " << line_no
                          << ": a route for 0.0.0.0/0 would undo the default route "
                          << "through the TUN\n";
                return false;
            }
            // The address bits beyond the prefix are dropped, as the kernel does.
            rule.addr &= mask_of(rule.prefix);
            if (overlaps(rule.addr, rule.prefix, overlay_addr, overlay_prefix)) {
                std::cerr << tag << " Bypass file line " << line_no << ": " << args[0]
                          << " overlaps the overlay " << overlay_cidr
                          << "; the overlay must stay on the TUN\n";
                return false;
            }
            rule.cidr = cidr_text(rule.addr, rule.prefix);
        } else if (kind == "in" || kind == "out" || kind == "in6" || kind == "out6") {
            rule.kind = kind == "in"  ? BypassRule::IN  :
                        kind == "out" ? BypassRule::OUT :
                        kind == "in6" ? BypassRule::IN6 : BypassRule::OUT6;
            if (args.empty()) {
                std::cerr << tag << " Bypass file line " << line_no << ": '" << kind
                          << "' needs iptables arguments\n";
                return false;
            }
            rule.args = args;
        } else {
            std::cerr << tag << " Bypass file line " << line_no << ": unknown line kind '"
                      << kind << "' (route, in, out, in6, out6)\n";
            return false;
        }
        fresh.push_back(rule);
    }
    rules = std::move(fresh);
    return true;
}

bool load_bypass_file(const std::string& path, const std::string& overlay_cidr,
                      std::vector<BypassRule>& rules, const char* tag) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << tag << " Cannot open bypass file " << path << "\n";
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return parse_bypass(buf.str(), overlay_cidr, rules, tag);
}

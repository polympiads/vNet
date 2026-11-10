#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * The bypass file of an agent (VNET_BYPASS_FILE): what stays on the
 * physical network while the agent is joined. Blank lines and lines
 * starting with '#' are ignored. Three kinds of lines:
 *
 *   route a.b.c.d/n
 *     The prefix keeps going through the physical gateway instead of the
 *     TUN, and under lockdown it is allowed in both directions on every
 *     port. A bare address is a /32.
 *
 *   out <iptables arguments>
 *   in  <iptables arguments>
 *     A rule appended to the lockdown chain VNET_OUT or VNET_IN, before
 *     the final DROP, exactly as written: "in -p tcp -s 10.20.0.0/16
 *     --dport 22443 -j ACCEPT". The arguments are split on whitespace and
 *     handed to iptables without a shell. Without lockdown they do
 *     nothing.
 *
 *   out6 <ip6tables arguments>
 *   in6  <ip6tables arguments>
 *     The same for IPv6, which lockdown otherwise blocks entirely.
 */
struct BypassRule {
    enum Kind { ROUTE, IN, OUT, IN6, OUT6 };
    Kind                     kind;
    /** ROUTE: the prefix as written, normalized to a.b.c.d/n. */
    std::string              cidr;
    /** ROUTE: the prefix in network order and its length. */
    uint32_t                 addr = 0;
    int                      prefix = 0;
    /** IN, OUT, IN6, OUT6: the iptables arguments. */
    std::vector<std::string> args;
};

/**
 * Parse the text of a bypass file.
 *
 * Every line must parse for the call to succeed; the first bad line is
 * reported on stderr with its number. A route prefix must not be
 * 0.0.0.0/0 (that would undo the default route through the TUN) and
 * must not overlap the overlay prefix (that would take the overlay off
 * the TUN).
 *
 * @param text          The whole file.
 * @param overlay_cidr  The overlay prefix, a.b.c.d/n.
 * @param rules         Receives the rules, in file order, on success.
 * @param tag           Log prefix such as "[Agent]".
 * @return true when every line parsed.
 */
bool parse_bypass(const std::string& text, const std::string& overlay_cidr,
                  std::vector<BypassRule>& rules, const char* tag);

/**
 * Read and parse a bypass file. A file that cannot be opened is an
 * error, so a wrong path cannot silently leave a machine without its
 * exceptions.
 */
bool load_bypass_file(const std::string& path, const std::string& overlay_cidr,
                      std::vector<BypassRule>& rules, const char* tag);

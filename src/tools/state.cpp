/*
 * vnet-state: print the conductor's state dump as text.
 *
 *   vnet-state [dump file] [auth file]
 *
 * The dump defaults to /var/vnet-dump.bin (VNET_DUMP_PATH). With the
 * conductor's auth file, the machines it expects that are not connected
 * are listed too, which is the question asked most during an event.
 */
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>

#include "state.pb.h"

static std::string when(int64_t ms) {
    if (ms <= 0) return "-";
    time_t secs = static_cast<time_t>(ms / 1000);
    char buf[32];
    struct tm tm_buf;
    localtime_r(&secs, &tm_buf);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

static std::string ipv4(uint32_t addr) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return buf;
}

int main(int argc, char** argv) {
    const char* env_path = std::getenv("VNET_DUMP_PATH");
    std::string path = argc > 1 ? argv[1] : (env_path ? env_path : "/var/vnet-dump.bin");

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return 1;
    }
    vnet::state::ConductorState state;
    if (!state.ParseFromIstream(&in)) {
        std::cerr << path << " is not a conductor state dump\n";
        return 1;
    }

    std::cout << "dump written " << when(state.dumped_at_ms()) << "\n\n";

    std::cout << "switches (" << state.switches_size() << ")\n";
    for (const auto& sw : state.switches()) {
        std::cout << "  " << sw.name() << "  " << sw.ipv4() << ":" << sw.port()
                  << "  network=" << sw.network()
                  << "  since " << when(sw.connected_at_ms()) << "\n";
    }

    std::vector<const vnet::state::AgentState*> agents;
    for (const auto& a : state.agents()) agents.push_back(&a);
    std::sort(agents.begin(), agents.end(),
              [](const auto* x, const auto* y) { return x->name() < y->name(); });
    std::cout << "\nagents (" << agents.size() << ")\n";
    for (const auto* a : agents) {
        std::cout << "  " << a->name() << "  " << ipv4(a->virtual_ipv4())
                  << "  on " << a->assigned_switch()
                  << "  since " << when(a->connected_at_ms()) << "\n";
    }

    if (state.pending_tokens_size() > 0) {
        std::cout << "\njoining (" << state.pending_tokens_size() << ")\n";
        for (const auto& t : state.pending_tokens())
            std::cout << "  " << t.agent_name() << " -> " << t.switch_name()
                      << "  since " << when(t.created_at_ms()) << "\n";
    }

    if (argc > 2) {
        std::ifstream auth(argv[2]);
        if (!auth) {
            std::cerr << "cannot open " << argv[2] << "\n";
            return 1;
        }
        std::set<std::string> have_switch, have_agent;
        for (const auto& sw : state.switches()) have_switch.insert(sw.name());
        for (const auto* a : agents) have_agent.insert(a->name());

        std::vector<std::string> missing;
        std::string line;
        while (std::getline(auth, line)) {
            std::istringstream iss(line);
            std::string role, name;
            if (!(iss >> role >> name) || role.empty() || role[0] == '#') continue;
            if (role == "switch" && !have_switch.count(name)) missing.push_back("switch " + name);
            if (role == "agent" && !have_agent.count(name)) missing.push_back("agent " + name);
        }
        std::cout << "\nexpected but not connected (" << missing.size() << ")\n";
        for (const auto& m : missing) std::cout << "  " << m << "\n";
    }
    return 0;
}

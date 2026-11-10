#pragma once

#include <string>
#include <vector>
#include <cstdint>

/**
 * Run a program with the given argument vector and wait for it.
 * The program is found through PATH; no shell is involved, so the
 * arguments are never interpreted.
 *
 * @param argv   Program name followed by its arguments.
 * @param tag    Log prefix such as "[Agent]" for the failure message.
 * @param quiet  Discard the program's output and log nothing on failure;
 *               for probes whose failure is an expected answer.
 * @return true when the program exited with status 0.
 */
bool run_program(const std::vector<std::string>& argv, const char* tag,
                 bool quiet = false);

/**
 * Read an integer setting from the environment.
 *
 * Unset: value is left unchanged and true is returned. Set and within
 * [min, max]: value is updated and true is returned. Otherwise an error
 * naming the variable is printed and false is returned.
 */
bool env_int(const char* name, long min, long max, long& value);

/**
 * Read a string setting from the environment, or def when unset.
 */
std::string env_or(const char* name, const std::string& def);

/**
 * A machine name: 1 to max_len characters from [A-Za-z0-9_-].
 * Agent names are limited to 10 characters, since "vnet-" plus the name
 * must fit the 15-character interface name limit.
 */
bool valid_name(const std::string& name, size_t max_len);

const size_t AGENT_NAME_MAX  = 10;
const size_t SWITCH_NAME_MAX = 32;

/**
 * Parse "a.b.c.d/n". addr is returned in network order, prefix in
 * [0, 32]. A bare address is accepted as /32.
 */
bool parse_cidr(const std::string& text, uint32_t& addr, int& prefix);

/**
 * The shared secret of this machine: VNET_AUTH_KEY, or the first line of
 * the file named by VNET_AUTH_KEY_FILE, or the command-line fallback.
 * Empty when none is available.
 */
std::string load_auth_key(const char* argv_key, const char* tag);

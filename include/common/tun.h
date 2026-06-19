#pragma once

#include <string>
#include <cstdint>

/**
 * @brief Open a TUN device and configure it.
 *
 * Creates a TUN interface with the given name, assigns the given
 * IPv4 address with a /24 prefix, and brings the interface up.
 *
 * @param name          Interface name (e.g. "vnet0"). Max 15 chars.
 * @param ipv4          Virtual IPv4 address in network order.
 * @param prefix_len    Subnet prefix length (default 24 = /24).
 * @return              File descriptor for the TUN device, or -1 on failure.
 */
int tun_open(const std::string& name, uint32_t ipv4, uint8_t prefix_len = 24);

/**
 * @brief Close a TUN device and bring the interface down.
 */
void tun_close(int fd, const std::string& name);

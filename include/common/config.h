#pragma once
#include <string>
#include <cstdint>

/**
 * @brief Generic machine configuration (IP + port).
 */
struct MachineConfig {
    std::string ip;   /**< The IP address of the conductor. */
    uint16_t port;    /**< The port number of the conductor. */
};

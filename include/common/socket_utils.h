#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include "common/config.h"
#include "vnet/protocol/header.hpp"
#include <google/protobuf/message.h>

/**
 * @brief Connect to another machine using MachineConfig (blocking).
 * @return socket fd on success, -1 on failure.
 */
int connect_to(const MachineConfig& mc);

/**
 * @brief Set a file descriptor to non-blocking mode.
 * @return true on success.
 */
bool set_nonblocking(int fd);

/**
 * @brief Read exactly n bytes from a socket (blocking).
 */
bool read_n_bytes(int sock, void* buffer, size_t n);

/**
 * @brief Write exactly n bytes to a socket.
 *        Handles EAGAIN for non-blocking fds via retry.
 */
bool write_n_bytes(int sock, const void* buffer, size_t n);

/**
 * @brief Send a protobuf message with the 6-byte packet header.
 *
 * Wire format: [4-byte payload_size (network order)]
 *              [2-byte packet_type  (network order)]
 *              [protobuf payload bytes]
 *
 * payload_size = sizeof(packet_type) + protobuf_size
 * but we store it as just protobuf_size since the header
 * struct stores both fields and NetQueue reads header first.
 *
 * IMPORTANT: payload_size in the header is the number of bytes
 * AFTER the 6-byte header, matching NetQueue::process().
 */
bool send_protobuf_packet(int sock, vnet::protocol::PacketType type,
                          const google::protobuf::Message& msg);

/**
 * @brief Read a protobuf message with the 6-byte packet header (blocking).
 *
 * Reads the full header, verifies the packet type, then reads
 * and parses the protobuf payload.
 *
 * @param expected_type  If set to a valid type, the function will
 *                       fail if the received type doesn't match.
 * @param received_type  If non-null, receives the actual packet type.
 */
bool read_protobuf_packet(int sock, vnet::protocol::PacketType expected_type,
                          google::protobuf::Message& msg);

/**
 * @brief Read a packet header and payload, returning the type.
 *        Useful when you don't know the packet type in advance.
 *
 * @param type     Receives the packet type.
 * @param buffer   Receives the payload bytes.
 * @param buf_size Receives the payload size.
 * @return true on success.
 */
bool read_raw_packet(int sock, vnet::protocol::PacketType& type,
                     std::vector<char>& buffer);

/**
 * @brief Send a heartbeat (header-only, zero-length payload).
 */
bool send_heartbeat(int sock);

/**
 * @brief Convert a network-order uint32_t IPv4 to string.
 */
std::string ipv4_to_string(uint32_t ipv4);

/**
 * @brief Convert an IPv4 string to network-order uint32_t.
 */
uint32_t string_to_ipv4(const std::string& ip);

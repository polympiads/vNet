#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <stdexcept>

#include "common/config.h"
#include "common/socket_utils.h"
#include "vnet/protocol/header.hpp"

using namespace vnet::protocol;

// ---------------------------------------------------------------------------
//  Low-level helpers
// ---------------------------------------------------------------------------

int connect_to(const MachineConfig& mc) {
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(mc.port);

    int rc = getaddrinfo(mc.ip.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0 || !result) {
        if (result) freeaddrinfo(result);
        return -1;
    }

    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(result);
        return -1;
    }

    if (connect(sock, result->ai_addr, result->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(result);
        return -1;
    }

    freeaddrinfo(result);
    return sock;
}

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool write_n_bytes(int sock, const void* buffer, size_t n) {
    size_t total = 0;
    const char* ptr = static_cast<const char*>(buffer);
    while (total < n) {
        ssize_t r = write(sock, ptr + total, n - total);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                usleep(200);          // brief yield, then retry
                continue;
            }
            return false;
        }
        if (r == 0) return false;
        total += r;
    }
    return true;
}

bool read_n_bytes(int sock, void* buffer, size_t n) {
    size_t total = 0;
    char* ptr = static_cast<char*>(buffer);
    while (total < n) {
        ssize_t r = read(sock, ptr + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;               // EAGAIN = not ready, treat as error in blocking context
        }
        if (r == 0) return false;       // EOF
        total += r;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Packet framing — matches the 6-byte PacketHeader used by NetQueue
//
//  Wire layout:
//    [uint32_t  payload_size ]   4 bytes, network order
//    [uint16_t  packet_type  ]   2 bytes, network order
//    [          protobuf body]   payload_size bytes
//
//  payload_size is the number of bytes AFTER the 6-byte header,
//  which is exactly the serialized protobuf size.
// ---------------------------------------------------------------------------

bool send_protobuf_packet(int sock, PacketType type,
                          const google::protobuf::Message& msg) {
    std::string body;
    if (!msg.SerializeToString(&body)) return false;

    PacketHeader hdr(static_cast<uint32_t>(body.size()), type);

    // Write header (6 bytes)
    if (!write_n_bytes(sock, &hdr, PACKET_HEADER_SIZE)) return false;

    // Write protobuf payload
    if (!body.empty()) {
        if (!write_n_bytes(sock, body.data(), body.size())) return false;
    }

    return true;
}

bool read_protobuf_packet(int sock, PacketType expected_type,
                          google::protobuf::Message& msg) {
    // Read 6-byte header
    PacketHeader hdr;
    if (!read_n_bytes(sock, &hdr, PACKET_HEADER_SIZE)) return false;

    PacketType type = hdr.get_packet_type();
    uint32_t   len  = hdr.get_payload_size();

    // Skip heartbeats transparently — keep reading until we get the
    // expected type (or any non-heartbeat).
    while (type == PacketType::HEARTBEAT) {
        // Heartbeat has zero-length payload, but honour the field anyway
        if (len > 0) {
            std::vector<char> skip(len);
            if (!read_n_bytes(sock, skip.data(), len)) return false;
        }
        if (!read_n_bytes(sock, &hdr, PACKET_HEADER_SIZE)) return false;
        type = hdr.get_packet_type();
        len  = hdr.get_payload_size();
    }

    if (type != expected_type) return false;

    if (len == 0) {
        msg.Clear();
        return true;
    }

    std::vector<char> buf(len);
    if (!read_n_bytes(sock, buf.data(), len)) return false;

    return msg.ParseFromArray(buf.data(), buf.size());
}

bool read_raw_packet(int sock, PacketType& type, std::vector<char>& buffer) {
    PacketHeader hdr;
    if (!read_n_bytes(sock, &hdr, PACKET_HEADER_SIZE)) return false;

    type          = hdr.get_packet_type();
    uint32_t len  = hdr.get_payload_size();

    buffer.resize(len);
    if (len > 0) {
        if (!read_n_bytes(sock, buffer.data(), len)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Heartbeat — header only, zero-length payload
// ---------------------------------------------------------------------------

bool send_heartbeat(int sock) {
    PacketHeader hdr(0, PacketType::HEARTBEAT);
    return write_n_bytes(sock, &hdr, PACKET_HEADER_SIZE);
}

// ---------------------------------------------------------------------------
//  IPv4 conversion helpers
// ---------------------------------------------------------------------------

std::string ipv4_to_string(uint32_t ipv4) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ipv4, buf, INET_ADDRSTRLEN);
    return std::string(buf);
}

uint32_t string_to_ipv4(const std::string& ip) {
    uint32_t addr = 0;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        throw std::runtime_error("Invalid IPv4 string: " + ip);
    }
    return addr;
}

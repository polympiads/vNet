#include "common/tun.h"

#include <string>
#include <cstring>
#include <cstdint>
#include <iostream>

#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>

int tun_open(const std::string& name, uint32_t ipv4, uint8_t prefix_len) {
    // 1. Open the TUN device file ---
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("[TUN] open /dev/net/tun");
        return -1;
    }

    // 2. Configure as TUN (no packet info header) ---
    struct ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("[TUN] ioctl TUNSETIFF");
        close(fd);
        return -1;
    }

    // 3. Assign IPv4 address via a temporary socket ---
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[TUN] socket");
        close(fd);
        return -1;
    }

    // Set IP address
    struct ifreq req_addr{};
    strncpy(req_addr.ifr_name, name.c_str(), IFNAMSIZ - 1);
    auto* addr = reinterpret_cast<sockaddr_in*>(&req_addr.ifr_addr);
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = ipv4;

    if (ioctl(sock, SIOCSIFADDR, &req_addr) < 0) {
        perror("[TUN] ioctl SIOCSIFADDR");
        close(sock);
        close(fd);
        return -1;
    }

    // Set netmask
    struct ifreq req_mask{};
    strncpy(req_mask.ifr_name, name.c_str(), IFNAMSIZ - 1);
    auto* mask = reinterpret_cast<sockaddr_in*>(&req_mask.ifr_netmask);
    mask->sin_family = AF_INET;

    uint32_t netmask = prefix_len == 0 ? 0 : htonl(~((1u << (32 - prefix_len)) - 1));
    mask->sin_addr.s_addr = netmask;

    if (ioctl(sock, SIOCSIFNETMASK, &req_mask) < 0) {
        perror("[TUN] ioctl SIOCSIFNETMASK");
        close(sock);
        close(fd);
        return -1;
    }

    // Bring interface up
    struct ifreq req_flags{};
    strncpy(req_flags.ifr_name, name.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFFLAGS, &req_flags) < 0) {
        perror("[TUN] ioctl SIOCGIFFLAGS");
        close(sock);
        close(fd);
        return -1;
    }

    req_flags.ifr_flags |= IFF_UP | IFF_RUNNING;

    if (ioctl(sock, SIOCSIFFLAGS, &req_flags) < 0) {
        perror("[TUN] ioctl SIOCSIFFLAGS");
        close(sock);
        close(fd);
        return -1;
    }

    close(sock);

    struct in_addr addr_print;
    addr_print.s_addr = ipv4;
    std::cout << "[TUN] Interface " << name
          << " up with IP " << inet_ntoa(addr_print)
          << "/" << (int)prefix_len << "\n";

    return fd;
}

void tun_close(int fd, const std::string& name) {
    // Bring interface down
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        struct ifreq ifr{};
        strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            ifr.ifr_flags &= ~IFF_UP;
            ioctl(sock, SIOCSIFFLAGS, &ifr);
        }
        close(sock);
    }
    close(fd);
}

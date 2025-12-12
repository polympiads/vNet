
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstddef>

#include <map>
#include <queue>
#include <utility>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

struct tun_wrapped_data {
    int opp;

    std::queue<size_t> read_buffers;
};

std::map<int, tun_wrapped_data> tun_data_per_fd;

void* byte_array_dup(const void* src, size_t size) {
    if (src == NULL || size == 0) {
        return NULL;
    }

    void* dest = malloc(size);

    if (dest == NULL) {
        return NULL;
    }

    memcpy(dest, src, size);

    return dest;
}

char total_buffer[70'000];

extern "C" ssize_t __real_read(int fd, void* buf, size_t count);
extern "C" ssize_t __wrap_read(int fd, void* buf, size_t count) {
    auto it_tun_data = tun_data_per_fd.find(fd);
    if (it_tun_data == tun_data_per_fd.end()) {
        return __real_read(fd, buf, count);
    }

    tun_wrapped_data& data = (*it_tun_data).second;
    if (data.read_buffers.size() == 0) {
        errno = EAGAIN;
        return -1;
    }

    size_t length = data.read_buffers.front();
    data.read_buffers.pop();

    __real_read(fd, total_buffer, length);
    size_t res = 0;
    for (; res < count && res < length; res ++) {
        ((char*) buf)[res] = total_buffer[res];
    }

    return res;
}

extern "C" ssize_t __real_write(int fd, const void* buf, size_t count);
extern "C" ssize_t __wrap_write(int fd, const void* buf, size_t count) {
    auto it_tun_data = tun_data_per_fd.find(fd);
    if (it_tun_data == tun_data_per_fd.end()) {
        return __real_write(fd, buf, count);
    }

    tun_wrapped_data& data = (*it_tun_data).second;

    int opp = data.opp;
    tun_wrapped_data& opp_data = tun_data_per_fd[opp];

    opp_data.read_buffers.push( count );
    return __real_write(fd, buf, count);
}

int tun_wrapper_open (int sv[2]) {
    int res = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
    if (res == -1) return -1;

    tun_data_per_fd[sv[0]] = { sv[1], {} };
    tun_data_per_fd[sv[1]] = { sv[0], {} };
    return res;
}
void tun_wrapper_close (int tunfd) {
    auto it = tun_data_per_fd.find(tunfd);
    if (it == tun_data_per_fd.end()) return ;

    int oppfd = (*it).second.opp;
    tun_data_per_fd.erase(it);
    close(tunfd);

    tun_wrapper_close(oppfd);
}

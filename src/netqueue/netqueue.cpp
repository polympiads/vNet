#include "vnet/netqueue/netqueue.hpp"
#include "vnet/netqueue/fsm.hpp"
#include "vnet/protocol/header.hpp"

#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include <cerrno>
#include <openssl/ssl.h>

using namespace vnet::netqueue;

void _real_close (int fd) {
    close(fd);
}

static void free_tls(NetworkElement* element) {
    if (!element || !element->ssl) return;
    SSL_free(reinterpret_cast<SSL*>(element->ssl));
    element->ssl = nullptr;
}

static ssize_t io_read(NetworkElement* element, void* buf, size_t n) {
    if (!element->ssl)
        return ::read(element->fd, buf, n);
    SSL* ssl = reinterpret_cast<SSL*>(element->ssl);
    int r = SSL_read(ssl, buf, static_cast<int>(n));
    if (r > 0) return r;
    int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN) return 0;
    errno = EIO;
    return -1;
}

static ssize_t io_write(NetworkElement* element, const void* buf, size_t n) {
    if (!element->ssl)
        return ::write(element->fd, buf, n);
    SSL* ssl = reinterpret_cast<SSL*>(element->ssl);
    int r = SSL_write(ssl, buf, static_cast<int>(n));
    if (r > 0) return r;
    int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    errno = EIO;
    return -1;
}

NetworkElement* NetQueue::put_tun (int tun_fd, void* data) {
    return put(tun_fd, data, FiniteStateMachine::TUN_RECEIVE);
}
NetworkElement* NetQueue::put_sck (int sck_fd, void* data) {
    return put(sck_fd, data, FiniteStateMachine::SCK_HEADER);
}

NetworkElement* NetQueue::put (int fd, void* data, FiniteStateMachine state) {
    std::lock_guard<std::mutex> lock_fd_to_network_element (fd_to_network_element_mutex);

    NetworkElement* element;

    try {
        element = new NetworkElement(fd, data, state);
    } catch (...) {
        return nullptr;
    }

    struct epoll_event event;
    event.data.ptr = element;
    event.events   = EPOLLIN | EPOLLET | EPOLLONESHOT;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        delete element;
        return nullptr;
    }

    try {
        fd_to_network_element[fd] = element;
    } catch (...) {
        delete element;
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &event);
        return nullptr;
    }

    return element;
}

bool NetQueue::flush_write_buffer(NetworkElement* element) {
    auto& buf    = element->write_buffer;
    auto& offset = element->write_buffer_offset;

    while (offset < buf.size()) {
        ssize_t r = io_write(element,
                             buf.data() + offset,
                             buf.size() - offset);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Still can't write — keep EPOLLOUT registered
                return true;
            }
            return false; // real error - caller should close
        }
        if (r == 0) return false;
        offset += r;
    }

    // Buffer fully drained — clear it and remove EPOLLOUT
    buf.clear();
    offset = 0;

    struct epoll_event ev;
    ev.data.ptr = element;
    ev.events   = EPOLLIN | EPOLLET | EPOLLONESHOT;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, element->fd, &ev);

    return true;
}

bool NetQueue::send(int fd, const void* data, size_t n) {
    NetworkElement* element = get_network_element_from_fd(fd);
    if (!element) return false;

    const uint8_t* ptr   = static_cast<const uint8_t*>(data);
    size_t         total = 0;

    // If there's already buffered data, append to it to preserve order
    if (!element->write_buffer.empty()) {
        size_t already_buffered = element->write_buffer.size()
                                    - element->write_buffer_offset;
        if (already_buffered + n > WRITE_BUFFER_MAX) {
            element->state = SCK_ERROR;
            return false;
        }
        element->write_buffer.reserve(element->write_buffer.size() + n);
        for (size_t i = 0; i < n; i++) {
            element->write_buffer.push_back(ptr[i]);
        }
        return true;
    }

    // Try to write directly first
    while (total < n) {
        ssize_t r = io_write(element, ptr + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Buffer the remainder
                size_t remaining = n - total;
                if (remaining > WRITE_BUFFER_MAX) {
                    element->state = SCK_ERROR;
                    return false;
                }
                element->write_buffer.assign(ptr + total, ptr + n);
                element->write_buffer_offset = 0;

                // Register EPOLLOUT
                struct epoll_event ev;
                ev.data.ptr = element;
                ev.events   = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT;
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);

                return true;
            }
            return false;
        }
        if (r == 0) return false;
        total += r;
    }
    return true;
}

bool NetQueue::send(int fd, protocol::PacketType type,
                    const google::protobuf::Message& msg) {
    std::string body;
    if (!msg.SerializeToString(&body)) return false;

    protocol::PacketHeader hdr(static_cast<uint32_t>(body.size()), type);

    if (!send(fd, &hdr, protocol::PACKET_HEADER_SIZE)) return false;
    if (!body.empty()) {
        if (!send(fd, body.data(), body.size())) return false;
    }
    return true;
}

bool NetQueue::send_heartbeat(int fd) {
    protocol::PacketHeader hdr(0, protocol::PacketType::HEARTBEAT);
    return send(fd, &hdr, protocol::PACKET_HEADER_SIZE);
}

NetQueue::NetQueue (NetworkQueueHandler handler) : NetQueue(handler, -1) {}
NetQueue::NetQueue (NetworkQueueHandler handler, int epoll_timeout) {
    if (handler.onClose == nullptr || handler.onSocketReady == nullptr || handler.onTunReady == nullptr) {
        throw std::invalid_argument( "handlers should exist" );
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
        throw std::runtime_error( "could not create EPOLL file descriptor" );

    this->epoll_fd      = epoll_fd;
    this->epoll_timeout = epoll_timeout;
    this->handler       = handler;
}

void NetQueue::reap_closed() {
    for (NetworkElement* element : deferred_delete)
        delete element;
    deferred_delete.clear();
}

void NetQueue::drain_tls(int fd) {
    NetworkElement* element = get_network_element_from_fd(fd);
    if (!element || !element->ssl || element->dead) return;
    SSL* ssl = reinterpret_cast<SSL*>(element->ssl);
    if (SSL_pending(ssl) <= 0 && !SSL_has_pending(ssl)) return;

    process(element);
    if (element->dead) return;
    if (element->state == SCK_ERROR || element->state == TUN_ERROR)
        close(element, CLOSE_ERROR);
    else if (element->state == SCK_EOF || element->state == TUN_EOF)
        close(element, CLOSE_HANGUP);
}

void NetQueue::wait_and_process () {
    reap_closed();

    struct epoll_event events[EPOLL_MAX_EVENTS];
    int nb_events = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS, epoll_timeout);
    if (nb_events < 0) {
        return ;
    }

    for (int i_event = 0; i_event < nb_events; i_event ++) {
        NetworkElement* current_element = (NetworkElement*) events[i_event].data.ptr;
        if (current_element->dead)
            continue ;

        bool should_close = false;
        close_reason reason = CLOSE_ERROR;
        // A TLS write can need a read before it finishes. Finish that write
        // before SSL_read, or the session fails and the conductor drops the peer.
        if (!current_element->write_buffer.empty() &&
            (events[i_event].events & (EPOLLIN | EPOLLOUT))) {
            if (!flush_write_buffer(current_element)) {
                should_close = true;
                reason = CLOSE_ERROR;
            }
        }
        if (!should_close && current_element->write_buffer.empty() &&
            (events[i_event].events & EPOLLIN)) {
            process(current_element);
            if (current_element->dead)
                continue ;

            if (current_element->state == SCK_ERROR || current_element->state == TUN_ERROR) {
                should_close = true;
                reason = CLOSE_ERROR;
            } else if (current_element->state == SCK_EOF || current_element->state == TUN_EOF) {
                should_close = true;
                reason = CLOSE_HANGUP;
            }
        }
        if (current_element->dead)
            continue ;
        if (events[i_event].events & EPOLLERR) {
            should_close = true;
            reason = CLOSE_ERROR;
        }
        if (events[i_event].events & EPOLLHUP) {
            should_close = true;
            reason = CLOSE_HANGUP;
        }

        if (!should_close && !current_element->dead) {
            struct epoll_event event;
            event.data.ptr = current_element;
            event.events   = EPOLLIN | EPOLLET | EPOLLONESHOT;
            if (!current_element->write_buffer.empty()) {
                event.events |= EPOLLOUT;
            }
            if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, current_element->fd, &event) < 0) {
                should_close = true;
                reason = CLOSE_EPOLL;
            }
        }
        if (should_close)
            close(current_element, reason);
    }

    reap_closed();
}

void NetQueue::process (NetworkElement* element) {
    while (1) {
        switch (element->state) {
            case SCK_HEADER: {
                ssize_t nb_rem  = protocol::PACKET_HEADER_SIZE - element->net_buffer_used;
                ssize_t nb_read = io_read(element, element->net_buffer + element->net_buffer_used, nb_rem);
                if (nb_read < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        return ;
                    
                    element->state = SCK_ERROR;
                    break ;
                }
                if (nb_read == 0) {
                    element->state = SCK_EOF;
                    break ;
                }

                element->net_buffer_used += nb_read;
                if (element->net_buffer_used == protocol::PACKET_HEADER_SIZE) {
                    element->state = SCK_PAYLOAD;
                }
                
                break ;
            }
            case SCK_PAYLOAD: {
                protocol::PacketHeader* header = (protocol::PacketHeader*) element->net_buffer;
                
                size_t  buffer_size = header->get_payload_size() + protocol::PACKET_HEADER_SIZE;
                ssize_t number_read = io_read(element, element->net_buffer + element->net_buffer_used, buffer_size - element->net_buffer_used);
                if (number_read < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        return ;
                    
                    element->state = SCK_ERROR;
                    break ;
                }

                element->net_buffer_used += number_read;
                if (element->net_buffer_used == buffer_size) {
                    element->state = SCK_READY;
                }

                break ;
            }
            case SCK_READY: {
                socket_data sck_data;
                sck_data.fd = element->fd;
                sck_data.ptr_data = element->ptr;

                sck_data.net_element = element;

                sck_data.full_buffer = element->net_buffer;
                sck_data.full_buffer_size = element->net_buffer_used;

                protocol::PacketHeader* header = (protocol::PacketHeader*) element->net_buffer;

                sck_data.packet_type   = header->get_packet_type();
                sck_data.payload_size  = header->get_payload_size();
                sck_data.packet_buffer = element->net_buffer + sizeof(protocol::PacketHeader);

                handler.onSocketReady(handler.ptr_data, sck_data);

                element->net_buffer_used = 0;
                element->state = SCK_HEADER;
                break ;
            }
            case SCK_ERROR:
                return ;
            case SCK_EOF:
                return ;
            
            case TUN_RECEIVE: {
                ssize_t nb_read = io_read(element, element->net_buffer, NET_BUFFER_SIZE);
                if (nb_read < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        return ;
                    
                    element->state = TUN_ERROR;
                    break ;
                }
                if (nb_read == 0) {
                    element->state = TUN_EOF;
                    break ;
                }

                element->net_buffer_used = nb_read;
                element->state = TUN_READY;

                break ;
            }
            case TUN_READY: {
                tun_data data;
                data.fd = element->fd;
                data.ptr_data = element->ptr;
                data.net_element = element;
                data.ip_buffer = element->net_buffer;
                data.ip_buffer_size = element->net_buffer_used;
                
                handler.onTunReady(handler.ptr_data, data);
                
                element->state = TUN_RECEIVE;

                break ;
            }
            case TUN_EOF:
                return ;
            case TUN_ERROR:
                return ;

            default:
                return ;
        }
    }
}

NetQueue::~NetQueue () {
    if (epoll_fd < 0) return ;

    for (std::pair<int, NetworkElement*> fd_with_network_element : fd_to_network_element) {
        free_tls(fd_with_network_element.second);
        _real_close(fd_with_network_element.first);
        delete fd_with_network_element.second;
    }

    fd_to_network_element.clear();
    reap_closed();

    _real_close(epoll_fd);
}

NetworkElement* NetQueue::get_network_element_from_fd (int fd) {
    std::lock_guard<std::mutex> lock (fd_to_network_element_mutex);

    auto it = fd_to_network_element.find(fd);
    if (it == fd_to_network_element.end()) {
        return nullptr;
    }

    return (*it).second;
}

void NetQueue::close (int fd) {
    NetworkElement* element = get_network_element_from_fd(fd);
    if (element == nullptr) {
        return ;
    }

    close(element, CLOSE_ERROR);
}
void NetQueue::close (NetworkElement* element, close_reason reason) {
    if (element == nullptr) {
        return ;
    }

    {
        std::lock_guard<std::mutex> lock (fd_to_network_element_mutex);
        if (element->dead) {
            return ;
        }
        auto it = fd_to_network_element.find(element->fd);
        // The caller still owns an element that was never put in the queue.
        if (it == fd_to_network_element.end() || (*it).second != element) {
            return ;
        }
        element->dead = true;
    }

    close_data data;
    data.fd = element->fd;
    data.net_element = element;
    data.ptr_data = element->ptr;
    data.reason = reason;
    // onClose may send on another socket, which takes the same mutex.
    handler.onClose(handler.ptr_data, data);
    free_tls(element);

    std::lock_guard<std::mutex> lock (fd_to_network_element_mutex);

    int fd = element->fd;
    auto it = fd_to_network_element.find(fd);
    if (it != fd_to_network_element.end() && (*it).second == element) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        _real_close(fd);
        fd_to_network_element.erase(it);
    }

    deferred_delete.push_back(element);
}
/**
 * transport.c - Transport layer implementation
 */
#include "client/transport.h"
#include "protocol/binary/binary_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ============================================================
 * Utility Functions
 * ============================================================ */

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool set_recv_timeout(int fd, uint32_t timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

bool set_send_timeout(int fd, uint32_t timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

bool resolve_host(const char* host, struct sockaddr_in* addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    
    /* Try direct IP first */
    if (inet_pton(AF_INET, host, &addr->sin_addr) == 1) {
        return true;
    }
    
    /* Fall back to DNS lookup */
    struct hostent* he = gethostbyname(host);
    if (he == NULL || he->h_addr_list[0] == NULL) {
        return false;
    }
    
    memcpy(&addr->sin_addr, he->h_addr_list[0], sizeof(addr->sin_addr));
    return true;
}

/* ============================================================
 * Transport Implementation
 * ============================================================ */

void transport_init(transport_t* t) {
    memset(t, 0, sizeof(*t));
    t->sock_fd = -1;
    t->state = CONN_STATE_DISCONNECTED;
    t->connect_timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;
    t->recv_timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;
    framing_read_state_init(&t->read_state);
}

/**
 * Try TCP connection with timeout
 */
static bool try_tcp_connect(transport_t* t, uint32_t timeout_ms) {
    t->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->sock_fd < 0) {
        return false;
    }
    
    /* Set non-blocking for connect with timeout */
    if (!set_nonblocking(t->sock_fd)) {
        close(t->sock_fd);
        t->sock_fd = -1;
        return false;
    }
    
    /* Attempt connect (will return immediately with EINPROGRESS) */
    int ret = connect(t->sock_fd, (struct sockaddr*)&t->server_addr, 
                      sizeof(t->server_addr));
    
    if (ret == 0) {
        /* Connected immediately (rare but possible on localhost) */
        t->type = TRANSPORT_TCP;
        t->state = CONN_STATE_CONNECTED;
        return true;
    }
    
    if (errno != EINPROGRESS) {
        close(t->sock_fd);
        t->sock_fd = -1;
        return false;
    }
    
    /* Wait for connection with timeout */
    struct pollfd pfd = {
        .fd = t->sock_fd,
        .events = POLLOUT,
        .revents = 0
    };
    
    ret = poll(&pfd, 1, (int)timeout_ms);
    
    if (ret <= 0) {
        /* Timeout or error */
        close(t->sock_fd);
        t->sock_fd = -1;
        return false;
    }
    
    /* Check if connection succeeded */
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(t->sock_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        close(t->sock_fd);
        t->sock_fd = -1;
        return false;
    }
    
    /* Success! Keep socket in non-blocking mode for recv */
    /* We'll use poll() for timeouts instead of SO_RCVTIMEO */
    
    /* Set TCP_NODELAY for low latency */
    int nodelay = 1;
    setsockopt(t->sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    
    /* Initialize framing state */
    framing_read_state_init(&t->read_state);
    
    t->type = TRANSPORT_TCP;
    t->state = CONN_STATE_CONNECTED;
    return true;
}

/**
 * Setup UDP socket
 */
static bool setup_udp(transport_t* t) {
    t->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (t->sock_fd < 0) {
        return false;
    }
    
    /* Set non-blocking */
    set_nonblocking(t->sock_fd);
    
    t->type = TRANSPORT_UDP;
    t->state = CONN_STATE_CONNECTED;
    return true;
}

bool transport_connect(transport_t* t,
                       const char* host,
                       uint16_t port,
                       transport_type_t type,
                       uint32_t timeout_ms) {
    /* Store config */
    strncpy(t->host, host, sizeof(t->host) - 1);
    t->host[sizeof(t->host) - 1] = '\0';
    t->port = port;
    t->connect_timeout_ms = timeout_ms;
    
    /* Resolve hostname */
    if (!resolve_host(host, &t->server_addr)) {
        fprintf(stderr, "Failed to resolve host: %s\n", host);
        return false;
    }
    t->server_addr.sin_port = htons(port);
    
    t->state = CONN_STATE_CONNECTING;
    
    if (type == TRANSPORT_TCP) {
        /* Force TCP */
        return try_tcp_connect(t, timeout_ms);
    }
    else if (type == TRANSPORT_UDP) {
        /* Force UDP */
        return setup_udp(t);
    }
    else {
        /* Auto-detect: try TCP first, fall back to UDP */
        if (try_tcp_connect(t, timeout_ms)) {
            return true;
        }
        
        /* TCP failed, try UDP */
        return setup_udp(t);
    }
}

void transport_disconnect(transport_t* t) {
    if (t->sock_fd >= 0) {
        close(t->sock_fd);
        t->sock_fd = -1;
    }
    t->state = CONN_STATE_DISCONNECTED;
}

bool transport_send(transport_t* t, const void* data, size_t len) {
    if (t->state != CONN_STATE_CONNECTED || t->sock_fd < 0) {
        return false;
    }
    
    if (t->type == TRANSPORT_TCP) {
        /* TCP: use length-prefix framing */
        char framed[FRAMING_BUFFER_SIZE];
        size_t framed_len;
        
        if (!frame_message((const char*)data, len, framed, &framed_len)) {
            return false;
        }
        
        /* Send framed message - may need multiple writes for non-blocking socket */
        size_t total_sent = 0;
        while (total_sent < framed_len) {
            ssize_t sent = send(t->sock_fd, framed + total_sent, 
                               framed_len - total_sent, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Would block - poll for writability */
                    struct pollfd pfd = { .fd = t->sock_fd, .events = POLLOUT };
                    if (poll(&pfd, 1, 100) <= 0) {  /* 100ms - allows Ctrl+C */
                        return false;  /* Timeout or error */
                    }
                    continue;
                }
                return false;
            }
            total_sent += sent;
        }
        
        t->bytes_sent += framed_len;
    }
    else {
        /* UDP: send as single datagram */
        ssize_t sent = sendto(t->sock_fd, data, len, 0,
                              (struct sockaddr*)&t->server_addr,
                              sizeof(t->server_addr));
        if (sent != (ssize_t)len) {
            return false;
        }
        
        t->bytes_sent += len;
    }
    
    t->messages_sent++;
    return true;
}

bool transport_recv(transport_t* t,
                    void* buffer,
                    size_t buffer_size,
                    size_t* out_len,
                    int timeout_ms) {
    if (t->state != CONN_STATE_CONNECTED || t->sock_fd < 0) {
        return false;
    }
    
    if (t->type == TRANSPORT_TCP) {
        /* TCP: handle framing */
        
        /* First check if we already have a complete message buffered */
        const char* msg_data;
        size_t msg_len;
        framing_result_t result = framing_read_extract(&t->read_state, &msg_data, &msg_len);
        
        if (result == FRAMING_MESSAGE_READY) {
            if (msg_len > buffer_size) {
                return false;  /* Buffer too small */
            }
            memcpy(buffer, msg_data, msg_len);
            *out_len = msg_len;
            t->messages_received++;
            return true;
        }
        
        /* Need to read more data - use poll() for timeout */
        if (timeout_ms >= 0) {
            struct pollfd pfd = { .fd = t->sock_fd, .events = POLLIN, .revents = 0 };
            int poll_ret = poll(&pfd, 1, timeout_ms);
            
            if (poll_ret <= 0) {
                /* Timeout (0) or error (-1) */
                return false;
            }
            
            if (!(pfd.revents & POLLIN)) {
                /* No data available (POLLHUP, POLLERR, etc.) */
                if (pfd.revents & (POLLHUP | POLLERR)) {
                    t->state = CONN_STATE_DISCONNECTED;
                }
                return false;
            }
        }
        
        /* Now do non-blocking recv */
        char temp_buf[TRANSPORT_RECV_BUFFER_SIZE];
        ssize_t received = recv(t->sock_fd, temp_buf, sizeof(temp_buf), MSG_DONTWAIT);
        
        if (received <= 0) {
            if (received == 0) {
                t->state = CONN_STATE_DISCONNECTED;  /* Server closed */
            }
            /* EAGAIN/EWOULDBLOCK is normal for non-blocking - just no data yet */
            return false;
        }
        
        t->bytes_received += received;
        
        /* Append to framing buffer */
        framing_read_append(&t->read_state, temp_buf, (size_t)received);
        
        /* Try to extract message again */
        result = framing_read_extract(&t->read_state, &msg_data, &msg_len);
        
        if (result == FRAMING_MESSAGE_READY) {
            if (msg_len > buffer_size) {
                return false;
            }
            memcpy(buffer, msg_data, msg_len);
            *out_len = msg_len;
            t->messages_received++;
            return true;
        }
        
        return false;  /* Still incomplete */
    }
    else {
        /* UDP: receive single datagram */
        
        /* Use poll() for timeout */
        if (timeout_ms >= 0) {
            struct pollfd pfd = { .fd = t->sock_fd, .events = POLLIN, .revents = 0 };
            int poll_ret = poll(&pfd, 1, timeout_ms);
            
            if (poll_ret <= 0 || !(pfd.revents & POLLIN)) {
                return false;
            }
        }
        
        ssize_t received = recvfrom(t->sock_fd, buffer, buffer_size, MSG_DONTWAIT, NULL, NULL);
        
        if (received <= 0) {
            return false;
        }
        
        *out_len = (size_t)received;
        t->bytes_received += received;
        t->messages_received++;
        return true;
    }
}

bool transport_has_data(transport_t* t) {
    if (t->state != CONN_STATE_CONNECTED || t->sock_fd < 0) {
        return false;
    }
    
    /* Check framing buffer first (TCP only) */
    if (t->type == TRANSPORT_TCP && framing_read_has_data(&t->read_state)) {
        return true;
    }
    
    /* Check socket */
    struct pollfd pfd = {
        .fd = t->sock_fd,
        .events = POLLIN,
        .revents = 0
    };
    
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

transport_type_t transport_get_type(const transport_t* t) {
    return t->type;
}

bool transport_is_connected(const transport_t* t) {
    return t->state == CONN_STATE_CONNECTED;
}

int transport_get_fd(const transport_t* t) {
    return t->sock_fd;
}

void transport_print_stats(const transport_t* t) {
    printf("Transport Statistics:\n");
    printf("  Type:              %s\n", transport_type_str(t->type));
    printf("  State:             %s\n", 
           t->state == CONN_STATE_CONNECTED ? "connected" : "disconnected");
    printf("  Messages sent:     %lu\n", (unsigned long)t->messages_sent);
    printf("  Messages received: %lu\n", (unsigned long)t->messages_received);
    printf("  Bytes sent:        %lu\n", (unsigned long)t->bytes_sent);
    printf("  Bytes received:    %lu\n", (unsigned long)t->bytes_received);
}

/* ============================================================
 * Multicast Receiver Implementation
 * ============================================================ */

void multicast_receiver_init(multicast_receiver_t* m) {
    memset(m, 0, sizeof(*m));
    m->sock_fd = -1;
    m->joined = false;
}

bool multicast_receiver_join(multicast_receiver_t* m,
                             const char* group,
                             uint16_t port) {
    strncpy(m->group, group, sizeof(m->group) - 1);
    m->group[sizeof(m->group) - 1] = '\0';
    m->port = port;
    
    /* Create UDP socket */
    m->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m->sock_fd < 0) {
        perror("multicast socket");
        return false;
    }
    
    /* Allow multiple subscribers on same machine */
    int reuse = 1;
    if (setsockopt(m->sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("SO_REUSEADDR");
        close(m->sock_fd);
        m->sock_fd = -1;
        return false;
    }
    
#ifdef SO_REUSEPORT
    if (setsockopt(m->sock_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        /* Not fatal - SO_REUSEPORT not available on all systems */
    }
#endif
    
    /* Bind to multicast port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(m->sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("multicast bind");
        close(m->sock_fd);
        m->sock_fd = -1;
        return false;
    }
    
    /* Join multicast group */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(group);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    
    if (setsockopt(m->sock_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("IP_ADD_MEMBERSHIP");
        close(m->sock_fd);
        m->sock_fd = -1;
        return false;
    }
    
    /* Set non-blocking */
    set_nonblocking(m->sock_fd);
    
    m->joined = true;
    return true;
}

void multicast_receiver_leave(multicast_receiver_t* m) {
    if (m->sock_fd >= 0) {
        if (m->joined) {
            struct ip_mreq mreq;
            mreq.imr_multiaddr.s_addr = inet_addr(m->group);
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            setsockopt(m->sock_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        }
        close(m->sock_fd);
        m->sock_fd = -1;
    }
    m->joined = false;
}

bool multicast_receiver_recv(multicast_receiver_t* m,
                             void* buffer,
                             size_t buffer_size,
                             size_t* out_len,
                             int timeout_ms) {
    if (!m->joined || m->sock_fd < 0) {
        return false;
    }
    
    /* Use poll() for timeout */
    if (timeout_ms >= 0) {
        struct pollfd pfd = { .fd = m->sock_fd, .events = POLLIN, .revents = 0 };
        int poll_ret = poll(&pfd, 1, timeout_ms);
        
        if (poll_ret <= 0 || !(pfd.revents & POLLIN)) {
            return false;
        }
    }
    
    ssize_t received = recvfrom(m->sock_fd, buffer, buffer_size, MSG_DONTWAIT, NULL, NULL);
    
    if (received <= 0) {
        return false;
    }
    
    *out_len = (size_t)received;
    m->packets_received++;
    m->bytes_received += received;
    return true;
}

int multicast_receiver_get_fd(const multicast_receiver_t* m) {
    return m->sock_fd;
}

void multicast_receiver_print_stats(const multicast_receiver_t* m) {
    printf("Multicast Statistics:\n");
    printf("  Group:             %s:%u\n", m->group, m->port);
    printf("  Joined:            %s\n", m->joined ? "yes" : "no");
    printf("  Packets received:  %lu\n", (unsigned long)m->packets_received);
    printf("  Bytes received:    %lu\n", (unsigned long)m->bytes_received);
}

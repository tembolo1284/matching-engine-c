/**
 * transport.h - Transport layer abstraction for matching engine client
 *
 * Provides unified interface for TCP and UDP transports with:
 *   - Auto-detection (try TCP first, fall back to UDP)
 *   - TCP length-prefix framing (reuses server's message_framing.h)
 *   - UDP datagram mode
 *   - Multicast subscription for market data
 *
 * Reuses:
 *   - network/message_framing.h (TCP framing protocol)
 *   - protocol/binary/binary_protocol.h (format detection)
 */

#ifndef CLIENT_TRANSPORT_H
#define CLIENT_TRANSPORT_H

#include "client/client_config.h"
#include "network/message_framing.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Constants
 * ============================================================ */

#define TRANSPORT_MAX_MESSAGE_SIZE  MAX_FRAMED_MESSAGE_SIZE
#define TRANSPORT_RECV_BUFFER_SIZE  8192

/* ============================================================
 * Transport Handle
 * ============================================================ */

/**
 * Transport connection state
 */
typedef struct {
    /* Configuration */
    transport_type_t    type;           /* TCP or UDP */
    char                host[CLIENT_MAX_HOST_LEN];
    uint16_t            port;
    
    /* Socket state */
    int                 sock_fd;
    struct sockaddr_in  server_addr;
    conn_state_t        state;
    
    /* TCP framing state (only used for TCP) */
    framing_read_state_t    read_state;
    framing_write_state_t   write_state;
    
    /* Receive buffer (for UDP or raw reads) */
    char                recv_buffer[TRANSPORT_RECV_BUFFER_SIZE];
    size_t              recv_buffer_len;
    
    /* Timeouts */
    uint32_t            connect_timeout_ms;
    uint32_t            recv_timeout_ms;
    
    /* Statistics */
    uint64_t            bytes_sent;
    uint64_t            bytes_received;
    uint64_t            messages_sent;
    uint64_t            messages_received;
    
} transport_t;

/**
 * Multicast receiver state
 */
typedef struct {
    int                 sock_fd;
    char                group[64];
    uint16_t            port;
    struct sockaddr_in  group_addr;
    bool                joined;
    
    /* Receive buffer */
    char                recv_buffer[TRANSPORT_RECV_BUFFER_SIZE];
    
    /* Statistics */
    uint64_t            packets_received;
    uint64_t            bytes_received;
    
} multicast_receiver_t;

/* ============================================================
 * Transport API
 * ============================================================ */

/**
 * Initialize transport (doesn't connect yet)
 */
void transport_init(transport_t* t);

/**
 * Connect to server
 * 
 * If type is TRANSPORT_AUTO:
 *   1. Try TCP connect with short timeout
 *   2. If TCP fails, use UDP
 * 
 * @param t         Transport handle
 * @param host      Server hostname or IP
 * @param port      Server port
 * @param type      Transport type (AUTO, TCP, or UDP)
 * @param timeout_ms Connection timeout in milliseconds
 * @return          true on success
 */
bool transport_connect(transport_t* t,
                       const char* host,
                       uint16_t port,
                       transport_type_t type,
                       uint32_t timeout_ms);

/**
 * Disconnect and cleanup
 */
void transport_disconnect(transport_t* t);

/**
 * Send raw data
 * 
 * For TCP: automatically adds length-prefix framing
 * For UDP: sends as single datagram
 * 
 * @param t         Transport handle
 * @param data      Data to send
 * @param len       Data length
 * @return          true on success
 */
bool transport_send(transport_t* t, const void* data, size_t len);

/**
 * Receive a complete message
 * 
 * For TCP: handles framing, returns complete message
 * For UDP: returns single datagram
 * 
 * @param t         Transport handle
 * @param buffer    Buffer to receive into
 * @param buffer_size Size of buffer
 * @param out_len   Actual message length received
 * @param timeout_ms Receive timeout (0 = non-blocking, -1 = block forever)
 * @return          true if message received, false on timeout/error
 */
bool transport_recv(transport_t* t,
                    void* buffer,
                    size_t buffer_size,
                    size_t* out_len,
                    int timeout_ms);

/**
 * Check if data is available to read (non-blocking)
 */
bool transport_has_data(transport_t* t);

/**
 * Get detected/actual transport type
 */
transport_type_t transport_get_type(const transport_t* t);

/**
 * Check if connected
 */
bool transport_is_connected(const transport_t* t);

/**
 * Get file descriptor (for poll/select)
 */
int transport_get_fd(const transport_t* t);

/**
 * Print transport statistics
 */
void transport_print_stats(const transport_t* t);

/* ============================================================
 * Multicast Receiver API
 * ============================================================ */

/**
 * Initialize multicast receiver
 */
void multicast_receiver_init(multicast_receiver_t* m);

/**
 * Join multicast group
 * 
 * @param m         Multicast receiver handle
 * @param group     Multicast group address (e.g., "239.255.0.1")
 * @param port      Multicast port
 * @return          true on success
 */
bool multicast_receiver_join(multicast_receiver_t* m,
                             const char* group,
                             uint16_t port);

/**
 * Leave multicast group and cleanup
 */
void multicast_receiver_leave(multicast_receiver_t* m);

/**
 * Receive multicast packet
 * 
 * @param m         Multicast receiver handle
 * @param buffer    Buffer to receive into
 * @param buffer_size Size of buffer
 * @param out_len   Actual packet length received
 * @param timeout_ms Receive timeout (0 = non-blocking, -1 = block forever)
 * @return          true if packet received
 */
bool multicast_receiver_recv(multicast_receiver_t* m,
                             void* buffer,
                             size_t buffer_size,
                             size_t* out_len,
                             int timeout_ms);

/**
 * Get file descriptor (for poll/select)
 */
int multicast_receiver_get_fd(const multicast_receiver_t* m);

/**
 * Print multicast statistics
 */
void multicast_receiver_print_stats(const multicast_receiver_t* m);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/**
 * Set socket to non-blocking mode
 */
bool set_nonblocking(int fd);

/**
 * Set socket receive timeout
 */
bool set_recv_timeout(int fd, uint32_t timeout_ms);

/**
 * Set socket send timeout
 */
bool set_send_timeout(int fd, uint32_t timeout_ms);

/**
 * Resolve hostname to IPv4 address
 * 
 * @param host      Hostname or IP string
 * @param addr      Output address structure
 * @return          true on success
 */
bool resolve_host(const char* host, struct sockaddr_in* addr);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_TRANSPORT_H */

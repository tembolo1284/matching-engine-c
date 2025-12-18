#ifndef TCP_CONNECTION_H
#define TCP_CONNECTION_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <netinet/in.h>
#include <pthread.h>
#include <assert.h>

#include "network/message_framing.h"
#include "threading/lockfree_queue.h"
#include "protocol/message_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * TCP Connection Management
 *
 * Manages per-client connection state for multi-client TCP server.
 * Each client gets:
 *   - Unique client_id (used for routing)
 *   - Dedicated output queue (lock-free SPSC)
 *   - Read/write state for framing protocol
 *   - Statistics tracking
 *
 * Cache Optimization:
 *   - Hot fields (socket_fd, client_id, active) grouped together
 *   - Output queue pointer for cache-friendly access
 *   - Statistics at end (cold path)
 *
 * Kernel Bypass Notes:
 *   - socket_fd abstraction point for DPDK rx/tx queue index
 *   - Output queue compatible with zero-copy designs
 */

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Maximum simultaneous TCP clients */
#define MAX_TCP_CLIENTS 100

/* Output queue capacity per client */
#define TCP_CLIENT_OUTPUT_QUEUE_SIZE 524288

/* Socket optimization flags */
#define TCP_OPT_NODELAY     (1 << 0)  /* Disable Nagle's algorithm */
#define TCP_OPT_QUICKACK    (1 << 1)  /* Disable delayed ACKs (Linux) */
#define TCP_OPT_BUSY_POLL   (1 << 2)  /* Enable busy polling (Linux) */
#define TCP_OPT_LOW_LATENCY (TCP_OPT_NODELAY | TCP_OPT_QUICKACK | TCP_OPT_BUSY_POLL)

/* Declare lock-free queue type for output messages */
DECLARE_LOCKFREE_QUEUE(output_msg_t, output_queue)

/* ============================================================================
 * Per-Client Connection State
 * ============================================================================ */

/**
 * TCP Client State
 *
 * Layout optimized for cache efficiency:
 *   - Hot fields (checked every event loop iteration) first
 *   - Queue pointer for O(1) access
 *   - Framing state (accessed on I/O)
 *   - Statistics last (cold path)
 */
typedef struct {
    /* === Cache Line 1: Hot fields (frequently accessed) === */
    int socket_fd;                      /* Client socket (-1 if inactive) */
    uint32_t client_id;                 /* Unique ID (1-based, 0 = invalid) */
    bool active;                        /* true if connected */
    bool has_pending_write;             /* true if write_state is active */
    uint8_t _pad1[2];                   /* Alignment padding */
    struct sockaddr_in addr;            /* Client address (16 bytes) */

    /* Output queue (lock-free) - Producer: output_router, Consumer: tcp_listener */
    output_queue_t output_queue;

    /* === Framing State (accessed on I/O) === */
    framing_read_state_t read_state;    /* Handles partial reads */
    framing_write_state_t write_state;  /* Handles partial writes */

    /* === Statistics (cold path) === */
    time_t connected_at;                /* Connection timestamp */
    uint64_t messages_received;         /* Total messages received */
    uint64_t messages_sent;             /* Total messages sent */
    uint64_t bytes_received;            /* Total bytes received */
    uint64_t bytes_sent;                /* Total bytes sent */

} tcp_client_t;

/* ============================================================================
 * Client Registry
 * ============================================================================ */

/**
 * Client Registry - manages all active connections
 *
 * Thread Safety:
 *   - Lock protects add/remove operations only
 *   - Individual client access is lock-free after lookup
 *   - Statistics access is lock-free (atomic or single-writer)
 */
typedef struct {
    tcp_client_t clients[MAX_TCP_CLIENTS];
    size_t active_count;                /* Number of active clients */
    pthread_mutex_t lock;               /* Protects client add/remove */
} tcp_client_registry_t;

/* ============================================================================
 * Initialization and Cleanup
 * ============================================================================ */

/**
 * Initialize the client registry
 *
 * @param registry Registry to initialize
 *
 * Preconditions:
 * - registry != NULL
 */
void tcp_client_registry_init(tcp_client_registry_t* registry);

/**
 * Destroy the client registry
 *
 * Closes all active connections and frees resources.
 *
 * @param registry Registry to destroy
 */
void tcp_client_registry_destroy(tcp_client_registry_t* registry);

/* ============================================================================
 * Client Management
 * ============================================================================ */

/**
 * Add a new client connection
 *
 * @param registry   Client registry
 * @param socket_fd  Client socket file descriptor
 * @param addr       Client address
 * @param client_id  [OUT] Assigned client ID
 * @return true on success, false if at capacity
 *
 * Preconditions:
 * - registry != NULL
 * - socket_fd >= 0
 * - client_id != NULL
 *
 * Thread Safety: Uses internal lock
 */
bool tcp_client_add(tcp_client_registry_t* registry,
                    int socket_fd,
                    struct sockaddr_in addr,
                    uint32_t* client_id);

/**
 * Remove a client connection
 *
 * Closes socket and marks client as inactive. Does NOT cancel orders -
 * that's handled by the processor thread.
 *
 * @param registry  Client registry
 * @param client_id Client to remove
 *
 * Thread Safety: Uses internal lock
 */
void tcp_client_remove(tcp_client_registry_t* registry,
                       uint32_t client_id);

/**
 * Get client by ID
 *
 * @param registry  Client registry
 * @param client_id Client ID
 * @return Pointer to client, or NULL if not found/inactive
 *
 * Thread Safety: Lock-free read after registry lock released
 */
tcp_client_t* tcp_client_get(tcp_client_registry_t* registry,
                             uint32_t client_id);

/**
 * Get count of active clients
 *
 * Thread Safety: Uses internal lock
 */
size_t tcp_client_get_active_count(tcp_client_registry_t* registry);

/* ============================================================================
 * Output Queue Operations
 * ============================================================================ */

/**
 * Enqueue an output message for a specific client
 *
 * @param client Client to send to
 * @param msg    Message to send
 * @return true on success, false if queue full
 *
 * Preconditions:
 * - client != NULL
 * - client->active == true
 * - msg != NULL
 *
 * Thread Safety: Lock-free (SPSC queue)
 */
bool tcp_client_enqueue_output(tcp_client_t* client,
                               const output_msg_t* msg);

/**
 * Dequeue an output message from a client's queue
 *
 * @param client Client to read from
 * @param msg    [OUT] Dequeued message
 * @return true if message available, false if queue empty
 *
 * Preconditions:
 * - client != NULL
 * - msg != NULL
 *
 * Thread Safety: Lock-free (SPSC queue)
 */
bool tcp_client_dequeue_output(tcp_client_t* client,
                               output_msg_t* msg);

/* ============================================================================
 * Bulk Operations
 * ============================================================================ */

/**
 * Disconnect all clients
 *
 * Called during graceful shutdown. Returns list of client_ids
 * that had outstanding orders (for cancellation).
 *
 * @param registry    Client registry
 * @param client_ids  [OUT] Array to store disconnected client IDs
 * @param max_clients Size of client_ids array
 * @return Number of clients disconnected
 *
 * Thread Safety: Uses internal lock
 */
size_t tcp_client_disconnect_all(tcp_client_registry_t* registry,
                                 uint32_t* client_ids,
                                 size_t max_clients);

/* ============================================================================
 * Socket Optimization (for kernel path - not used with DPDK)
 * ============================================================================ */

/**
 * Apply low-latency socket options
 *
 * Sets TCP_NODELAY, TCP_QUICKACK (Linux), and SO_BUSY_POLL (Linux).
 * Call after accept() for each client socket.
 *
 * @param socket_fd Socket to optimize
 * @param flags     Bitmask of TCP_OPT_* flags
 * @return true if all requested options were set
 *
 * Note: This is a no-op when using kernel bypass (DPDK/AF_XDP)
 */
bool tcp_socket_set_low_latency(int socket_fd, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif /* TCP_CONNECTION_H */

#ifndef TCP_CONNECTION_H
#define TCP_CONNECTION_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <netinet/in.h>
#include <pthread.h>
#include "network/message_framing.h"
#include "lockfree_queue.h"
#include "message_types.h"

/**
 * TCP Connection Management
 * 
 * Manages per-client connection state for multi-client TCP server.
 * Each client gets:
 *   - Unique client_id (used for routing)
 *   - Dedicated output queue (lock-free SPSC)
 *   - Read/write state for framing protocol
 *   - Statistics tracking
 */

// Maximum simultaneous TCP clients
#define MAX_TCP_CLIENTS 100

// Output queue capacity per client
#define TCP_CLIENT_OUTPUT_QUEUE_SIZE 4096

// Declare lock-free queue type for output messages
DECLARE_LOCKFREE_QUEUE(output_msg_t, output_queue)

/**
 * Per-client connection state
 */
typedef struct {
    // Network state
    int socket_fd;                      // Client socket (-1 if inactive)
    uint32_t client_id;                 // Unique ID (1-based, 0 = invalid)
    struct sockaddr_in addr;            // Client address
    bool active;                        // true if connected
    
    // Input framing state
    framing_read_state_t read_state;    // Handles partial reads
    
    // Output queue (lock-free)
    output_queue_t output_queue;        // Producer: output_router
                                        // Consumer: tcp_listener
    
    // Output framing state (for current message being written)
    framing_write_state_t write_state;  // Handles partial writes
    bool has_pending_write;             // true if write_state is active
    
    // Statistics
    time_t connected_at;                // Connection timestamp
    uint64_t messages_received;         // Total messages received
    uint64_t messages_sent;             // Total messages sent
    uint64_t bytes_received;            // Total bytes received
    uint64_t bytes_sent;                // Total bytes sent
    
} tcp_client_t;

/**
 * Client registry - manages all active connections
 */
typedef struct {
    tcp_client_t clients[MAX_TCP_CLIENTS];
    size_t active_count;                // Number of active clients
    pthread_mutex_t lock;               // Protects client add/remove
} tcp_client_registry_t;

/**
 * Initialize the client registry
 */
void tcp_client_registry_init(tcp_client_registry_t* registry);

/**
 * Destroy the client registry
 */
void tcp_client_registry_destroy(tcp_client_registry_t* registry);

/**
 * Add a new client connection
 * 
 * @param registry Client registry
 * @param socket_fd Client socket file descriptor
 * @param addr Client address
 * @param client_id [OUT] Assigned client ID
 * @return true on success, false if at capacity
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
 * @param registry Client registry
 * @param client_id Client to remove
 */
void tcp_client_remove(tcp_client_registry_t* registry,
                       uint32_t client_id);

/**
 * Get client by ID
 * 
 * @param registry Client registry
 * @param client_id Client ID
 * @return Pointer to client, or NULL if not found/inactive
 */
tcp_client_t* tcp_client_get(tcp_client_registry_t* registry,
                             uint32_t client_id);

/**
 * Get count of active clients
 */
size_t tcp_client_get_active_count(tcp_client_registry_t* registry);

/**
 * Enqueue an output message for a specific client
 * 
 * @param client Client to send to
 * @param msg Message to send
 * @return true on success, false if queue full
 */
bool tcp_client_enqueue_output(tcp_client_t* client,
                               const output_msg_t* msg);

/**
 * Dequeue an output message from a client's queue
 * 
 * @param client Client to read from
 * @param msg [OUT] Dequeued message
 * @return true if message available, false if queue empty
 */
bool tcp_client_dequeue_output(tcp_client_t* client,
                               output_msg_t* msg);

/**
 * Disconnect all clients
 * 
 * Called during graceful shutdown. Returns list of client_ids
 * that had outstanding orders (for cancellation).
 * 
 * @param registry Client registry
 * @param client_ids [OUT] Array to store disconnected client IDs
 * @param max_clients Size of client_ids array
 * @return Number of clients disconnected
 */
size_t tcp_client_disconnect_all(tcp_client_registry_t* registry,
                                 uint32_t* client_ids,
                                 size_t max_clients);

#endif // TCP_CONNECTION_H

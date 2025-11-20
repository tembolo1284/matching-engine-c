#ifndef TCP_LISTENER_H
#define TCP_LISTENER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "network/tcp_connection.h"
#include "protocol/message_types_extended.h"
#include "threading/lockfree_queue.h"

/**
 * TCP Listener Thread
 * 
 * Event-driven network I/O using epoll (Linux). Handles:
 *   - Accepting new client connections
 *   - Reading framed messages from all clients
 *   - Writing output messages to clients
 *   - Detecting client disconnections
 * 
 * Architecture:
 *   Single thread uses epoll to multiplex I/O across all clients
 *   No thread-per-client overhead
 *   Scales efficiently to 100+ clients
 * 
 * Flow:
 *   Accept → Read (framed) → Parse → Enqueue to processor
 *   Processor → Output router → Client queues → Write (framed)
 */

// Declare input envelope queue
DECLARE_LOCKFREE_QUEUE(input_msg_envelope_t, input_envelope_queue)

/**
 * TCP listener configuration
 */
typedef struct {
    uint16_t port;                          // TCP port to listen on
    int listen_backlog;                     // accept() queue depth
    bool use_binary_output;                 // Binary vs CSV output format
} tcp_listener_config_t;

/**
 * TCP listener context
 */
typedef struct {
    // Configuration
    tcp_listener_config_t config;
    
    // Network state
    int listen_fd;                          // Listening socket
    int epoll_fd;                           // epoll file descriptor
    
    // Client registry
    tcp_client_registry_t* client_registry;
    
    // Input queue (to processor)
    input_envelope_queue_t* input_queue;
    
    // Shutdown coordination
    atomic_bool* shutdown_flag;
    
    // Statistics
    uint64_t total_connections;             // Total connections accepted
    uint64_t total_messages_received;       // Total messages received
    uint64_t total_messages_sent;           // Total messages sent
    uint64_t total_bytes_received;          // Total bytes received
    uint64_t total_bytes_sent;              // Total bytes sent
    
} tcp_listener_context_t;

/**
 * Initialize TCP listener context
 * 
 * @param ctx Context to initialize
 * @param config Configuration
 * @param client_registry Shared client registry
 * @param input_queue Input message queue (to processor)
 * @param shutdown_flag Shutdown coordination flag
 * @return true on success
 */
bool tcp_listener_init(tcp_listener_context_t* ctx,
                       const tcp_listener_config_t* config,
                       tcp_client_registry_t* client_registry,
                       input_envelope_queue_t* input_queue,
                       atomic_bool* shutdown_flag);

/**
 * Cleanup TCP listener context
 */
void tcp_listener_cleanup(tcp_listener_context_t* ctx);

/**
 * TCP listener thread entry point
 * 
 * @param arg Pointer to tcp_listener_context_t
 * @return NULL
 */
void* tcp_listener_thread(void* arg);

/**
 * Print TCP listener statistics
 */
void tcp_listener_print_stats(const tcp_listener_context_t* ctx);

#endif // TCP_LISTENER_H

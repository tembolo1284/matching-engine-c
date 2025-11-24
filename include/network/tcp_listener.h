#ifndef TCP_LISTENER_H
#define TCP_LISTENER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "network/tcp_connection.h"
#include "protocol/message_types_extended.h"
#include "threading/lockfree_queue.h"
#include "threading/queues.h"

/**
 * TCP Listener Thread
 * 
 * Event-driven network I/O using epoll (Linux) or kqueue (macOS/BSD). Handles:
 *   - Accepting new client connections
 *   - Reading framed messages from all clients
 *   - Writing output messages to clients
 *   - Detecting client disconnections
 * 
 * Updated for dual-processor support:
 *   - Routes messages by symbol to appropriate processor queue
 *   - A-M symbols → input_queue_0
 *   - N-Z symbols → input_queue_1
 *   - Flush commands → BOTH queues
 *   - Cancels without symbol → BOTH queues
 * 
 * Architecture:
 *   Single thread uses epoll/kqueue to multiplex I/O across all clients
 *   No thread-per-client overhead
 *   Scales efficiently to 100+ clients
 */

#define MAX_INPUT_QUEUES 2

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
    
    // Platform-specific event mechanism
#ifdef __linux__
    int epoll_fd;                           // epoll file descriptor (Linux)
#else
    int kqueue_fd;                          // kqueue file descriptor (macOS/BSD)
#endif
    
    // Client registry
    tcp_client_registry_t* client_registry;
    
    // Input queues - supports dual-processor mode
    input_envelope_queue_t* input_queues[MAX_INPUT_QUEUES];
    int num_input_queues;                   // 1 = single, 2 = dual
    
    // Legacy single-queue pointer for backward compatibility
    input_envelope_queue_t* input_queue;    // Points to input_queues[0]
    
    // Shutdown coordination
    atomic_bool* shutdown_flag;
    
    // Statistics
    uint64_t total_connections;             // Total connections accepted
    uint64_t total_messages_received;       // Total messages received
    uint64_t total_messages_sent;           // Total messages sent
    uint64_t total_bytes_received;          // Total bytes received
    uint64_t total_bytes_sent;              // Total bytes sent
    uint64_t messages_to_processor[MAX_INPUT_QUEUES];  // Per-processor stats
    
} tcp_listener_context_t;

/**
 * Initialize TCP listener context (single processor mode - backward compatible)
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
 * Initialize TCP listener context for dual-processor mode
 * 
 * @param ctx Context to initialize
 * @param config Configuration
 * @param client_registry Shared client registry
 * @param input_queue_0 Input queue for A-M symbols (processor 0)
 * @param input_queue_1 Input queue for N-Z symbols (processor 1)
 * @param shutdown_flag Shutdown coordination flag
 * @return true on success
 */
bool tcp_listener_init_dual(tcp_listener_context_t* ctx,
                            const tcp_listener_config_t* config,
                            tcp_client_registry_t* client_registry,
                            input_envelope_queue_t* input_queue_0,
                            input_envelope_queue_t* input_queue_1,
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

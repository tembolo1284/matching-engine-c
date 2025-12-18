#ifndef TCP_LISTENER_H
#define TCP_LISTENER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "network/tcp_connection.h"
#include "protocol/message_types_extended.h"
#include "threading/lockfree_queue.h"
#include "threading/queues.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * TCP Listener Thread
 *
 * Event-driven network I/O using epoll (Linux) or kqueue (macOS/BSD). Handles:
 *   - Accepting new client connections
 *   - Reading framed messages from all clients
 *   - Writing output messages to clients
 *   - Detecting client disconnections
 *
 * Dual-Processor Support:
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
 *
 * Kernel Bypass Integration Points:
 *   [KB-1] setup_listening_socket() → DPDK port init
 *   [KB-2] setup_event_loop() → DPDK poll mode / completion queue
 *   [KB-3] handle_client_read() → rte_eth_rx_burst() + packet parsing
 *   [KB-4] handle_client_write() → rte_eth_tx_burst()
 *   [KB-5] handle_new_connection() → N/A (connectionless with DPDK UDP)
 *
 * For TCP with DPDK, consider DPDK's TCP stack (F-Stack, mTCP) or
 * hybrid approach with kernel TCP + DPDK UDP.
 */

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define MAX_INPUT_QUEUES 2

/* Event loop tuning */
#define TCP_MAX_EVENTS          128     /* Max events per epoll_wait/kevent */
#define TCP_EVENT_TIMEOUT_MS    100     /* Timeout for event wait */
#define TCP_LISTEN_BACKLOG      128     /* Default accept() queue depth */

/* ============================================================================
 * Listener Configuration
 * ============================================================================ */

/**
 * TCP listener configuration
 */
typedef struct {
    uint16_t port;                      /* TCP port to listen on */
    int listen_backlog;                 /* accept() queue depth (0 = default) */
    bool use_binary_output;             /* Binary vs CSV output format */
} tcp_listener_config_t;

/* ============================================================================
 * Listener Context
 * ============================================================================ */

/**
 * TCP listener context
 *
 * Kernel Bypass Notes:
 * - listen_fd → DPDK port_id
 * - epoll_fd/kqueue_fd → N/A (poll mode)
 * - Statistics compatible with both paths
 */
typedef struct {
    /* Configuration */
    tcp_listener_config_t config;

    /* Network state */
    int listen_fd;                      /* Listening socket */

    /* Platform-specific event mechanism */
#ifdef __linux__
    int epoll_fd;                       /* epoll file descriptor (Linux) */
#else
    int kqueue_fd;                      /* kqueue file descriptor (macOS/BSD) */
#endif

    /* Client registry */
    tcp_client_registry_t* client_registry;

    /* Input queues - supports dual-processor mode */
    input_envelope_queue_t* input_queues[MAX_INPUT_QUEUES];
    int num_input_queues;               /* 1 = single, 2 = dual */

    /* Legacy single-queue pointer for backward compatibility */
    input_envelope_queue_t* input_queue; /* Points to input_queues[0] */

    /* Shutdown coordination */
    atomic_bool* shutdown_flag;

    /* Statistics (compatible with kernel bypass) */
    uint64_t total_connections;         /* Total connections accepted */
    uint64_t total_messages_received;   /* Total messages received */
    uint64_t total_messages_sent;       /* Total messages sent */
    uint64_t total_bytes_received;      /* Total bytes received */
    uint64_t total_bytes_sent;          /* Total bytes sent */
    uint64_t messages_to_processor[MAX_INPUT_QUEUES]; /* Per-processor stats */
    uint64_t parse_errors;              /* Message parse failures */
    uint64_t queue_full_drops;          /* Messages dropped due to full queue */

} tcp_listener_context_t;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize TCP listener context (single processor mode)
 *
 * @param ctx             Context to initialize
 * @param config          Configuration
 * @param client_registry Shared client registry
 * @param input_queue     Input message queue (to processor)
 * @param shutdown_flag   Shutdown coordination flag
 * @return true on success
 *
 * Preconditions:
 * - ctx != NULL
 * - config != NULL
 * - client_registry != NULL
 * - input_queue != NULL
 * - shutdown_flag != NULL
 */
bool tcp_listener_init(tcp_listener_context_t* ctx,
                       const tcp_listener_config_t* config,
                       tcp_client_registry_t* client_registry,
                       input_envelope_queue_t* input_queue,
                       atomic_bool* shutdown_flag);

/**
 * Initialize TCP listener context for dual-processor mode
 *
 * @param ctx             Context to initialize
 * @param config          Configuration
 * @param client_registry Shared client registry
 * @param input_queue_0   Input queue for A-M symbols (processor 0)
 * @param input_queue_1   Input queue for N-Z symbols (processor 1)
 * @param shutdown_flag   Shutdown coordination flag
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
 *
 * Closes sockets and frees event loop resources.
 */
void tcp_listener_cleanup(tcp_listener_context_t* ctx);

/* ============================================================================
 * Thread Entry Point
 * ============================================================================ */

/**
 * TCP listener thread entry point
 *
 * Runs the event loop until shutdown_flag is set.
 *
 * @param arg Pointer to tcp_listener_context_t
 * @return NULL
 */
void* tcp_listener_thread(void* arg);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Print TCP listener statistics
 */
void tcp_listener_print_stats(const tcp_listener_context_t* ctx);

/**
 * Reset TCP listener statistics
 */
void tcp_listener_reset_stats(tcp_listener_context_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* TCP_LISTENER_H */

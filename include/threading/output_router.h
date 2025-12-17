#ifndef OUTPUT_ROUTER_H
#define OUTPUT_ROUTER_H

/**
 * Output Router Thread
 *
 * Routes output messages from processor(s) to individual client queues.
 * In TCP mode, this performs per-client routing based on client_id.
 * In UDP mode, this can still be used but routes to a single stdout queue.
 *
 * Power of Ten Compliance:
 * - Rule 2: All loops bounded (ROUTER_BATCH_SIZE, MAX_OUTPUT_QUEUES, MAX_DRAIN_ITERATIONS)
 * - Rule 3: No dynamic allocation (fixed-size batch buffer)
 * - Rule 5: Assertions in all functions (see .c file)
 * - Rule 7: All return values checked
 *
 * Performance Notes:
 * - Uses batch dequeue to amortize atomic operations
 * - Statistics on separate cache line to prevent false sharing
 * - Round-robin polling for fairness across processor queues
 *
 * Dual-processor support:
 *   - Polls multiple output queues (one per processor)
 *   - Uses round-robin batching for fairness
 *   - Prevents starvation of either processor's output
 *
 * Integrated multicast:
 *   - Optionally broadcasts ALL messages to a multicast group
 *   - Multicast is in ADDITION to TCP unicast routing
 *   - Single thread handles both unicast and broadcast (no queue contention)
 *
 * Flow:
 *   Processor 0 → Output Queue 0 ┐
 *                                 ├→ Output Router ─┬→ Per-client queues (TCP)
 *   Processor 1 → Output Queue 1 ┘                  └→ Multicast group (subscribers)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <netinet/in.h>
#include "protocol/message_types_extended.h"
#include "threading/queues.h"
#include "network/tcp_connection.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define MAX_OUTPUT_QUEUES 2
#define ROUTER_BATCH_SIZE 32

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Multicast configuration (embedded in router)
 */
typedef struct {
    char multicast_group[64];   /* e.g., "239.255.0.1" */
    uint16_t port;              /* e.g., 5000 */
    uint8_t ttl;                /* 1=subnet, 32=site, 255=global */
    bool use_binary_output;     /* true=binary protocol, false=CSV */
} output_router_mcast_config_t;

/**
 * Output router configuration
 */
typedef struct {
    bool tcp_mode;              /* true = route to clients, false = stdout */
} output_router_config_t;

/**
 * Output router statistics
 *
 * PERFORMANCE: Grouped in cache-line aligned struct to prevent
 * false sharing with other router context fields.
 */
typedef struct {
    uint64_t messages_routed;               /* Total messages routed to TCP clients */
    uint64_t messages_dropped;              /* Messages to disconnected clients */
    uint64_t messages_from_processor[MAX_OUTPUT_QUEUES];  /* Per-processor counts */
    uint64_t mcast_messages;                /* Messages broadcast to multicast */
    uint64_t mcast_errors;                  /* Multicast send errors */
} output_router_stats_t;

/**
 * Output router context
 *
 * Memory layout:
 * - Configuration and pointers first (read-mostly)
 * - Statistics on separate cache line (write-heavy)
 */
typedef struct {
    /* Configuration - read-only after init */
    output_router_config_t config;

    /* Client registry (for TCP mode) */
    tcp_client_registry_t* client_registry;

    /* Input queues (from processors) - supports dual-processor mode */
    output_envelope_queue_t* input_queues[MAX_OUTPUT_QUEUES];
    int num_input_queues;                   /* 1 = single, 2 = dual */

    /* Legacy single-queue pointer for backward compatibility */
    output_envelope_queue_t* input_queue;   /* Points to input_queues[0] */

    /* Shutdown coordination */
    atomic_bool* shutdown_flag;

    /* Multicast support (integrated into router) */
    bool mcast_enabled;
    output_router_mcast_config_t mcast_config;
    int mcast_sockfd;
    struct sockaddr_in mcast_addr;

    /* Statistics - on separate cache line to prevent false sharing */
    alignas(CACHE_LINE_SIZE) uint64_t messages_routed;
    uint64_t messages_dropped;
    uint64_t messages_from_processor[MAX_OUTPUT_QUEUES];
    uint64_t mcast_messages;
    uint64_t mcast_errors;

} output_router_context_t;

/* ============================================================================
 * Initialization / Cleanup
 * ============================================================================ */

/**
 * Initialize output router context (single processor mode - backward compatible)
 *
 * @param ctx             Context to initialize (must not be NULL)
 * @param config          Configuration (must not be NULL)
 * @param client_registry Client registry (can be NULL for UDP mode)
 * @param input_queue     Output envelope queue from processor (must not be NULL)
 * @param shutdown_flag   Shutdown coordination flag (must not be NULL)
 * @return                true on success
 */
bool output_router_init(output_router_context_t* ctx,
                        const output_router_config_t* config,
                        tcp_client_registry_t* client_registry,
                        output_envelope_queue_t* input_queue,
                        atomic_bool* shutdown_flag);

/**
 * Initialize output router context for dual-processor mode
 *
 * @param ctx             Context to initialize (must not be NULL)
 * @param config          Configuration (must not be NULL)
 * @param client_registry Client registry (can be NULL for UDP mode)
 * @param input_queue_0   Output envelope queue from processor 0 (must not be NULL)
 * @param input_queue_1   Output envelope queue from processor 1 (must not be NULL)
 * @param shutdown_flag   Shutdown coordination flag (must not be NULL)
 * @return                true on success
 */
bool output_router_init_dual(output_router_context_t* ctx,
                             const output_router_config_t* config,
                             tcp_client_registry_t* client_registry,
                             output_envelope_queue_t* input_queue_0,
                             output_envelope_queue_t* input_queue_1,
                             atomic_bool* shutdown_flag);

/**
 * Enable multicast broadcasting on the router
 *
 * Call this AFTER output_router_init/init_dual but BEFORE starting the thread.
 * When enabled, the router broadcasts ALL output messages to the multicast
 * group in addition to routing them to individual TCP clients.
 *
 * @param ctx             Router context (must be initialized)
 * @param multicast_group Multicast group address (e.g., "239.255.0.1")
 * @param port            Multicast port (e.g., 5000)
 * @param ttl             Time-to-live (1=local subnet, 32=site, 255=global)
 * @param use_binary      true=binary protocol, false=CSV text
 * @return                true on success, false on socket error
 */
bool output_router_enable_multicast(output_router_context_t* ctx,
                                    const char* multicast_group,
                                    uint16_t port,
                                    uint8_t ttl,
                                    bool use_binary);

/**
 * Cleanup output router context
 *
 * Closes multicast socket if open.
 *
 * @param ctx  Router context (must not be NULL)
 */
void output_router_cleanup(output_router_context_t* ctx);

/* ============================================================================
 * Thread Entry Point
 * ============================================================================ */

/**
 * Output router thread entry point
 *
 * @param arg  Pointer to output_router_context_t
 * @return     NULL
 */
void* output_router_thread(void* arg);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Print output router statistics to stderr
 *
 * @param ctx  Router context (must not be NULL)
 */
void output_router_print_stats(const output_router_context_t* ctx);

#endif /* OUTPUT_ROUTER_H */

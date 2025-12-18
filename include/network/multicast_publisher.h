#ifndef MULTICAST_PUBLISHER_H
#define MULTICAST_PUBLISHER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <netinet/in.h>

#include "protocol/message_types_extended.h"
#include "threading/queues.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Multicast Publisher Thread
 *
 * Broadcasts output messages to a UDP multicast group for market data distribution.
 * This is how real exchanges (CME, NASDAQ, ICE) distribute market data - one send,
 * thousands of subscribers receive simultaneously.
 *
 * Features:
 *   - UDP multicast broadcasting (true one-to-many)
 *   - Supports both CSV and Binary protocols
 *   - Works in single and dual-processor modes
 *   - Round-robin batching across multiple processor output queues
 *   - Configurable multicast group and port
 *   - Sequence numbers for gap detection
 *   - Statistics tracking
 *
 * Architecture:
 *   Processor 0 → Output Queue 0 ┐
 *                                 ├→ Multicast Publisher → Multicast Group
 *   Processor 1 → Output Queue 1 ┘                       (239.255.0.1:5000)
 *                                                              ↓
 *                                                   ┌───────────┼───────────┐
 *                                                   ↓           ↓           ↓
 *                                              Subscriber 1  Subscriber 2  Subscriber N
 *
 * Usage:
 *   ./matching_engine --tcp --multicast 239.255.0.1:5000
 *
 *   In another terminal (same or different machine):
 *   ./multicast_subscriber 239.255.0.1 5000
 *
 * Benefits:
 *   - Zero server overhead per subscriber (vs N × TCP sends)
 *   - Unlimited subscribers (network handles distribution)
 *   - Consistent latency (no head-of-line blocking)
 *   - Industry standard (real exchange architecture)
 *
 * Kernel Bypass Integration Points:
 *   [KB-1] multicast_publisher_setup_socket() → DPDK port init + multicast config
 *   [KB-2] sendto() → rte_eth_tx_burst() with multicast MAC
 *   [KB-3] Batching already implemented - compatible with DPDK burst model
 *
 * For DPDK multicast, the NIC handles replication at hardware level,
 * providing even better performance than kernel multicast.
 */

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define MAX_MULTICAST_PACKET_SIZE   65507
#define MULTICAST_BATCH_SIZE        32
#define MAX_OUTPUT_QUEUES_MCAST     2
#define MULTICAST_GROUP_MAX_LEN     64

/* Default TTL values */
#define MULTICAST_TTL_LOCAL         1       /* Same subnet only */
#define MULTICAST_TTL_SITE          32      /* Within organization */
#define MULTICAST_TTL_REGION        64      /* Regional */
#define MULTICAST_TTL_GLOBAL        255     /* Unrestricted */

/* ============================================================================
 * Configuration Structure
 * ============================================================================ */

/**
 * Multicast publisher configuration
 */
typedef struct {
    char multicast_group[MULTICAST_GROUP_MAX_LEN];  /* e.g., "239.255.0.1" */
    uint16_t port;                                   /* e.g., 5000 */
    bool use_binary_output;                          /* true = binary, false = CSV */
    uint8_t ttl;                                     /* Time-to-live (1-255) */
    bool loopback;                                   /* Receive own packets */
} multicast_publisher_config_t;

/* ============================================================================
 * Publisher Context
 * ============================================================================ */

/**
 * Multicast publisher context
 *
 * Kernel Bypass Notes:
 * - sockfd → DPDK port_id
 * - mcast_addr → Used for DPDK multicast MAC computation
 * - All statistics compatible with both paths
 */
typedef struct {
    /* Configuration */
    multicast_publisher_config_t config;

    /* Input queues (from processors) - supports dual-processor mode */
    output_envelope_queue_t* input_queues[MAX_OUTPUT_QUEUES_MCAST];
    int num_input_queues;               /* 1 = single, 2 = dual processor */

    /* Network state [KB-1] */
    int sockfd;                         /* UDP socket (-1 if not open) */
    struct sockaddr_in mcast_addr;      /* Multicast group address */

    /* Thread management */
    pthread_t thread;
    atomic_bool* shutdown_flag;
    atomic_bool started;

    /* Statistics (compatible with DPDK) */
    atomic_uint_fast64_t packets_sent;
    atomic_uint_fast64_t bytes_sent;
    atomic_uint_fast64_t messages_broadcast;
    atomic_uint_fast64_t messages_from_processor[MAX_OUTPUT_QUEUES_MCAST];
    atomic_uint_fast64_t sequence;          /* For gap detection */
    atomic_uint_fast64_t send_errors;
    atomic_uint_fast64_t format_errors;

} multicast_publisher_context_t;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize multicast publisher (single processor mode)
 *
 * @param ctx           Context to initialize
 * @param config        Configuration
 * @param input_queue   Output envelope queue (from processor)
 * @param shutdown_flag Shutdown coordination flag
 * @return true on success
 *
 * Preconditions:
 * - ctx != NULL
 * - config != NULL
 * - input_queue != NULL
 * - shutdown_flag != NULL
 * - config->multicast_group is valid multicast address (224.0.0.0-239.255.255.255)
 */
bool multicast_publisher_init(multicast_publisher_context_t* ctx,
                              const multicast_publisher_config_t* config,
                              output_envelope_queue_t* input_queue,
                              atomic_bool* shutdown_flag);

/**
 * Initialize multicast publisher for dual-processor mode
 *
 * @param ctx            Context to initialize
 * @param config         Configuration
 * @param input_queue_0  Output envelope queue from processor 0 (A-M)
 * @param input_queue_1  Output envelope queue from processor 1 (N-Z)
 * @param shutdown_flag  Shutdown coordination flag
 * @return true on success
 */
bool multicast_publisher_init_dual(multicast_publisher_context_t* ctx,
                                   const multicast_publisher_config_t* config,
                                   output_envelope_queue_t* input_queue_0,
                                   output_envelope_queue_t* input_queue_1,
                                   atomic_bool* shutdown_flag);

/**
 * Cleanup multicast publisher context
 *
 * Closes socket and releases resources.
 */
void multicast_publisher_cleanup(multicast_publisher_context_t* ctx);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/**
 * Start multicast publisher thread
 *
 * @param ctx Context (must be initialized)
 * @return true on success
 */
bool multicast_publisher_start(multicast_publisher_context_t* ctx);

/**
 * Stop multicast publisher thread
 *
 * Waits for thread to finish draining queues.
 */
void multicast_publisher_stop(multicast_publisher_context_t* ctx);

/**
 * Multicast publisher thread entry point
 *
 * @param arg Pointer to multicast_publisher_context_t
 * @return NULL
 */
void* multicast_publisher_thread(void* arg);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Print multicast publisher statistics
 */
void multicast_publisher_print_stats(const multicast_publisher_context_t* ctx);

/**
 * Reset multicast publisher statistics
 */
void multicast_publisher_reset_stats(multicast_publisher_context_t* ctx);

/**
 * Get current sequence number
 */
uint64_t multicast_publisher_get_sequence(const multicast_publisher_context_t* ctx);

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

/**
 * Setup multicast socket [KB-1]
 *
 * Creates UDP socket and configures for multicast transmission.
 */
bool multicast_publisher_setup_socket(multicast_publisher_context_t* ctx);

/**
 * Validate multicast address
 *
 * Checks if address is in valid multicast range (224.0.0.0 - 239.255.255.255)
 */
bool multicast_address_is_valid(const char* address);

#ifdef __cplusplus
}
#endif

#endif /* MULTICAST_PUBLISHER_H */

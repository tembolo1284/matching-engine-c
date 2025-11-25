#ifndef MULTICAST_PUBLISHER_H
#define MULTICAST_PUBLISHER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#include "protocol/message_types_extended.h"
#include "threading/queues.h"

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
 */

#define MAX_MULTICAST_PACKET_SIZE 65507
#define MULTICAST_BATCH_SIZE 32
#define MAX_OUTPUT_QUEUES_MCAST 2

/**
 * Multicast publisher configuration
 */
typedef struct {
    char multicast_group[64];       // Multicast group address (e.g., "239.255.0.1")
    uint16_t port;                  // Multicast port (e.g., 5000)
    bool use_binary_output;         // true = binary, false = CSV
    uint8_t ttl;                    // Time-to-live (1 = local, 255 = global)
} multicast_publisher_config_t;

/**
 * Multicast publisher context
 */
typedef struct {
    // Configuration
    multicast_publisher_config_t config;
    
    // Input queues (from processors) - supports dual-processor mode
    output_envelope_queue_t* input_queues[MAX_OUTPUT_QUEUES_MCAST];
    int num_input_queues;           // 1 = single, 2 = dual processor
    
    // Network state
    int sockfd;                     // UDP socket
    struct sockaddr_in mcast_addr;  // Multicast group address
    
    // Shutdown coordination
    atomic_bool* shutdown_flag;
    pthread_t thread;
    
    // Statistics
    atomic_uint_fast64_t packets_sent;
    atomic_uint_fast64_t messages_broadcast;
    atomic_uint_fast64_t messages_from_processor[MAX_OUTPUT_QUEUES_MCAST];
    atomic_uint_fast64_t sequence;  // Sequence number for gap detection
    atomic_uint_fast64_t send_errors;
    
} multicast_publisher_context_t;

/**
 * Initialize multicast publisher (single processor mode)
 * 
 * @param ctx Context to initialize
 * @param config Configuration
 * @param input_queue Output envelope queue (from processor)
 * @param shutdown_flag Shutdown coordination flag
 * @return true on success
 */
bool multicast_publisher_init(multicast_publisher_context_t* ctx,
                               const multicast_publisher_config_t* config,
                               output_envelope_queue_t* input_queue,
                               atomic_bool* shutdown_flag);

/**
 * Initialize multicast publisher for dual-processor mode
 * 
 * @param ctx Context to initialize
 * @param config Configuration
 * @param input_queue_0 Output envelope queue from processor 0 (A-M)
 * @param input_queue_1 Output envelope queue from processor 1 (N-Z)
 * @param shutdown_flag Shutdown coordination flag
 * @return true on success
 */
bool multicast_publisher_init_dual(multicast_publisher_context_t* ctx,
                                    const multicast_publisher_config_t* config,
                                    output_envelope_queue_t* input_queue_0,
                                    output_envelope_queue_t* input_queue_1,
                                    atomic_bool* shutdown_flag);

/**
 * Cleanup multicast publisher context
 */
void multicast_publisher_cleanup(multicast_publisher_context_t* ctx);

/**
 * Multicast publisher thread entry point
 * 
 * @param arg Pointer to multicast_publisher_context_t
 * @return NULL
 */
void* multicast_publisher_thread(void* arg);

/**
 * Print multicast publisher statistics
 */
void multicast_publisher_print_stats(const multicast_publisher_context_t* ctx);

/**
 * Setup multicast socket (internal)
 */
bool multicast_publisher_setup_socket(multicast_publisher_context_t* ctx);

#endif // MULTICAST_PUBLISHER_H

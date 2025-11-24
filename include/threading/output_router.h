#ifndef OUTPUT_ROUTER_H
#define OUTPUT_ROUTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "protocol/message_types_extended.h"
#include "threading/queues.h"
#include "network/tcp_connection.h"

/**
 * Output Router Thread
 * 
 * Routes output messages from processor(s) to individual client queues.
 * In TCP mode, this performs per-client routing based on client_id.
 * In UDP mode, this can still be used but routes to a single stdout queue.
 * 
 * Updated for dual-processor support:
 *   - Polls multiple output queues (one per processor)
 *   - Uses round-robin batching for fairness
 *   - Prevents starvation of either processor's output
 * 
 * Flow:
 *   Processor 0 → Output Queue 0 ┐
 *                                 ├→ Output Router → Per-client queues
 *   Processor 1 → Output Queue 1 ┘                  → TCP Listener writes
 * 
 * Responsibilities:
 *   - Dequeue output envelopes from all processor output queues
 *   - Route to appropriate client's output queue
 *   - Handle disconnected clients (drop messages)
 *   - Fair scheduling across multiple input queues
 */

#define MAX_OUTPUT_QUEUES 2
#define ROUTER_BATCH_SIZE 32

/**
 * Output router configuration
 */
typedef struct {
    bool tcp_mode;                          // true = route to clients, false = stdout
} output_router_config_t;

/**
 * Output router context
 */
typedef struct {
    // Configuration
    output_router_config_t config;
    
    // Client registry (for TCP mode)
    tcp_client_registry_t* client_registry;
    
    // Input queues (from processors) - supports dual-processor mode
    output_envelope_queue_t* input_queues[MAX_OUTPUT_QUEUES];
    int num_input_queues;                   // 1 = single, 2 = dual
    
    // Legacy single-queue pointer for backward compatibility
    output_envelope_queue_t* input_queue;   // Points to input_queues[0]
    
    // Shutdown coordination
    atomic_bool* shutdown_flag;
    
    // Statistics - overall
    uint64_t messages_routed;               // Total messages routed
    uint64_t messages_dropped;              // Messages to disconnected clients
    
    // Statistics - per processor
    uint64_t messages_from_processor[MAX_OUTPUT_QUEUES];
    
} output_router_context_t;

/**
 * Initialize output router context (single processor mode - backward compatible)
 * 
 * @param ctx Context to initialize
 * @param config Configuration
 * @param client_registry Client registry (can be NULL for UDP mode)
 * @param input_queue Output envelope queue (from processor)
 * @param shutdown_flag Shutdown coordination flag
 * @return true on success
 */
bool output_router_init(output_router_context_t* ctx,
                        const output_router_config_t* config,
                        tcp_client_registry_t* client_registry,
                        output_envelope_queue_t* input_queue,
                        atomic_bool* shutdown_flag);

/**
 * Initialize output router context for dual-processor mode
 * 
 * @param ctx Context to initialize
 * @param config Configuration
 * @param client_registry Client registry (can be NULL for UDP mode)
 * @param input_queue_0 Output envelope queue from processor 0 (A-M)
 * @param input_queue_1 Output envelope queue from processor 1 (N-Z)
 * @param shutdown_flag Shutdown coordination flag
 * @return true on success
 */
bool output_router_init_dual(output_router_context_t* ctx,
                             const output_router_config_t* config,
                             tcp_client_registry_t* client_registry,
                             output_envelope_queue_t* input_queue_0,
                             output_envelope_queue_t* input_queue_1,
                             atomic_bool* shutdown_flag);

/**
 * Cleanup output router context
 */
void output_router_cleanup(output_router_context_t* ctx);

/**
 * Output router thread entry point
 * 
 * @param arg Pointer to output_router_context_t
 * @return NULL
 */
void* output_router_thread(void* arg);

/**
 * Print output router statistics
 */
void output_router_print_stats(const output_router_context_t* ctx);

#endif // OUTPUT_ROUTER_H

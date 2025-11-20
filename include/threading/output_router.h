#ifndef OUTPUT_ROUTER_H
#define OUTPUT_ROUTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "network/tcp_connection.h"
#include "protocol/message_types_extended.h"
#include "threading/lockfree_queue.h"

/**
 * Output Router Thread
 * 
 * Routes output messages from the processor to individual client queues.
 * In TCP mode, this performs per-client routing based on client_id.
 * In UDP mode, this can still be used but routes to a single stdout queue.
 * 
 * Flow:
 *   Processor → Output Queue (envelopes) → Output Router → Per-client queues
 *                                                         → TCP Listener writes
 * 
 * Responsibilities:
 *   - Dequeue output envelopes from processor
 *   - Route to appropriate client's output queue
 *   - Handle disconnected clients (drop messages)
 *   - Handle flush: cancel all orders for disconnected clients
 */

// Declare output envelope queue
DECLARE_LOCKFREE_QUEUE(output_msg_envelope_t, output_envelope_queue)

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
    
    // Input queue (from processor)
    output_envelope_queue_t* input_queue;
    
    // Shutdown coordination
    atomic_bool* shutdown_flag;
    
    // Statistics
    uint64_t messages_routed;               // Total messages routed
    uint64_t messages_dropped;              // Messages to disconnected clients
    
} output_router_context_t;

/**
 * Initialize output router context
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

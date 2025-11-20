#ifndef MATCHING_ENGINE_PROCESSOR_H
#define MATCHING_ENGINE_PROCESSOR_H

#include "message_types_extended.h"
#include "core/matching_engine.h"
#include "threading/queues.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Processor - Thread 2: Process input messages through matching engine
 * 
 * Updated for TCP multi-client support:
 * - Now uses envelope types (input_msg_envelope_t, output_msg_envelope_t)
 * - Tracks client_id for routing (0 = UDP mode, >0 = TCP client ID)
 * - Routes trade messages to both buyer and seller
 * - Routes ack/cancel/TOB to originating client only
 * - Supports client disconnection with order cancellation
 *
 * Design decisions:
 * - Runs in separate pthread
 * - Pops envelopes from input queue
 * - Routes to matching engine
 * - Pushes output envelopes to output queue with routing info
 * - Graceful shutdown via atomic flag
 * - Batch processing for better throughput (32 messages per iteration)
 * - Adaptive sleep (1μs when active, 100μs when idle)
 */

#define PROCESSOR_BATCH_SIZE 32
#define PROCESSOR_ACTIVE_SLEEP_US 1
#define PROCESSOR_IDLE_SLEEP_US 100
#define PROCESSOR_IDLE_THRESHOLD 100

/**
 * Processor configuration
 */
typedef struct {
    bool tcp_mode;              // true = TCP mode (client routing)
                                // false = UDP mode (client_id = 0)
} processor_config_t;

/**
 * Processor state
 */
typedef struct {
    /* Configuration */
    processor_config_t config;
    
    /* Queues - NOW USE ENVELOPES */
    input_envelope_queue_t* input_queue;
    output_envelope_queue_t* output_queue;
    
    /* Matching engine */
    matching_engine_t engine;
    
    /* Thread management */
    pthread_t thread;
    atomic_bool running;
    atomic_bool started;
    
    /* Statistics */
    atomic_uint_fast64_t messages_processed;
    atomic_uint_fast64_t batches_processed;
    
} processor_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize processor
 * 
 * @param processor Processor instance
 * @param config Configuration (tcp_mode flag)
 * @param input_queue Input envelope queue
 * @param output_queue Output envelope queue
 */
void processor_init(processor_t* processor,
                    const processor_config_t* config,
                    input_envelope_queue_t* input_queue,
                    output_envelope_queue_t* output_queue);

/**
 * Destroy processor and cleanup resources
 */
void processor_destroy(processor_t* processor);

/**
 * Start processing (spawns thread)
 * Returns true on success, false on error
 */
bool processor_start(processor_t* processor);

/**
 * Stop processing (signals thread to exit and waits)
 */
void processor_stop(processor_t* processor);

/**
 * Check if thread is running
 */
bool processor_is_running(const processor_t* processor);

/**
 * Get statistics
 */
uint64_t processor_get_messages_processed(const processor_t* processor);
uint64_t processor_get_batches_processed(const processor_t* processor);

/**
 * Cancel all orders for a specific client (TCP mode only)
 * 
 * Called when a TCP client disconnects. Walks through all order books
 * and cancels orders where order->client_id matches the given client_id.
 * Generates cancel acknowledgements that are enqueued to the output queue.
 * 
 * @param processor Processor instance
 * @param client_id Client ID whose orders should be cancelled
 * @return Number of orders cancelled
 */
size_t processor_cancel_client_orders(processor_t* processor, uint32_t client_id);

/* ============================================================================
 * Internal Functions (used by thread)
 * ============================================================================ */

/**
 * Thread entry point
 */
void* processor_thread_func(void* arg);

/**
 * Process a single input envelope
 * 
 * @param processor Processor instance
 * @param envelope Input message envelope (contains msg + client_id)
 */
void processor_process_envelope(processor_t* processor, 
                                const input_msg_envelope_t* envelope);

/**
 * Process a batch of envelopes (up to BATCH_SIZE)
 * Returns number of messages processed
 */
size_t processor_process_batch(processor_t* processor);

/**
 * Route output messages to appropriate clients
 * 
 * For trades: Creates TWO envelopes (one for buyer, one for seller)
 * For acks/cancels/TOB: Creates ONE envelope for originating client
 * 
 * @param processor Processor instance
 * @param outputs Raw output messages from matching engine
 * @param output_count Number of output messages
 * @param originating_client_id Client that sent the original order
 */
void processor_route_outputs(processor_t* processor,
                             const output_msg_t* outputs,
                             size_t output_count,
                             uint32_t originating_client_id);

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_PROCESSOR_H */

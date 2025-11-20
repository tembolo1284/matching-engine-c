#ifndef MATCHING_ENGINE_PROCESSOR_H
#define MATCHING_ENGINE_PROCESSOR_H

#include "protocol/message_types_extended.h"
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
 */

#define PROCESSOR_BATCH_SIZE 32
#define PROCESSOR_SLEEP_US 1

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
    matching_engine_t* engine;

    /* Thread management */
    pthread_t thread;
    atomic_bool running;
    atomic_bool started;
    atomic_bool* shutdown_flag;

    /* Statistics */
    atomic_uint_fast64_t messages_processed;
    atomic_uint_fast64_t batches_processed;
    
    /* Sequence tracking */
    atomic_uint_fast64_t output_sequence;

} processor_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize processor
 */
bool processor_init(processor_t* processor,
                    const processor_config_t* config,
                    matching_engine_t* engine,
                    input_envelope_queue_t* input_queue,
                    output_envelope_queue_t* output_queue,
                    atomic_bool* shutdown_flag);

/**
 * Destroy processor and cleanup resources
 */
void processor_cleanup(processor_t* processor);

/**
 * Thread entry point
 */
void* processor_thread(void* arg);

/**
 * Print statistics
 */
void processor_print_stats(const processor_t* processor);

/**
 * Cancel all orders for a specific client (TCP mode only)
 */
void processor_cancel_client_orders(processor_t* processor, uint32_t client_id);

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_PROCESSOR_H */

#ifndef MATCHING_ENGINE_PROCESSOR_H
#define MATCHING_ENGINE_PROCESSOR_H

#include "message_types.h"
#include "matching_engine.h"
#include "lockfree_queue.h"
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
 * Design decisions:
 * - Runs in separate pthread
 * - Pops messages from input queue
 * - Routes to matching engine
 * - Pushes output messages to output queue
 * - Graceful shutdown via atomic flag
 * - Batch processing for better throughput (32 messages per iteration)
 * - Adaptive sleep (1μs when active, 100μs when idle)
 */

#define PROCESSOR_BATCH_SIZE 32
#define PROCESSOR_ACTIVE_SLEEP_US 1
#define PROCESSOR_IDLE_SLEEP_US 100
#define PROCESSOR_IDLE_THRESHOLD 100

/**
 * Processor state
 */
typedef struct {
    /* Queues */
    input_queue_t* input_queue;
    output_queue_t* output_queue;
    
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
 */
void processor_init(processor_t* processor, 
                    input_queue_t* input_queue,
                    output_queue_t* output_queue);

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

/* ============================================================================
 * Internal Functions (used by thread)
 * ============================================================================ */

/**
 * Thread entry point
 */
void* processor_thread_func(void* arg);

/**
 * Process a single input message
 */
void processor_process_message(processor_t* processor, const input_msg_t* msg);

/**
 * Process a batch of messages (up to BATCH_SIZE)
 * Returns number of messages processed
 */
size_t processor_process_batch(processor_t* processor);

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_PROCESSOR_H */

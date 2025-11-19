#ifndef MATCHING_ENGINE_OUTPUT_PUBLISHER_H
#define MATCHING_ENGINE_OUTPUT_PUBLISHER_H

#include "message_types.h"
#include "message_formatter.h"
#include "binary_message_formatter.h"
#include "queues.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OutputPublisher - Thread 3: Publish output messages to stdout
 * 
 * Design decisions:
 * - Runs in separate pthread
 * - Pops messages from output queue
 * - Formats and writes to stdout
 * - Graceful shutdown via atomic flag
 * - Flushes stdout after each message for real-time output
 * - Brief sleep (10μs) when queue is empty to avoid busy-waiting
 */

#define OUTPUT_SLEEP_US 1000
#define OUTPUT_IDLE_THRESHOLD 100
#define OUTPUT_IDLE_SLEEP_US 10000
#define OUTPUT_ACTIVE_SLEEP_US 100

/**
 * Output publisher state
 */
typedef struct {
    /* Input queue */
    output_queue_t* output_queue;
    
    /* Message formatter */
    message_formatter_t formatter;
    
    /* Thread management */
    pthread_t thread;
    atomic_bool running;
    atomic_bool started;
    
    /* Statistics */
    atomic_uint_fast64_t messages_published;

    message_formatter_t csv_formatter;
    binary_message_formatter_t binary_formatter;
    bool use_binary;  /* Output format flag */
} output_publisher_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize output publisher
 */
void output_publisher_init(output_publisher_t* publisher, 
                           output_queue_t* queue, 
                           bool use_binary);

/**
 * Destroy output publisher and cleanup resources
 */
void output_publisher_destroy(output_publisher_t* publisher);

/**
 * Start publishing (spawns thread)
 * Returns true on success, false on error
 */
bool output_publisher_start(output_publisher_t* publisher);

/**
 * Stop publishing (signals thread to exit, drains queue, and waits)
 */
void output_publisher_stop(output_publisher_t* publisher);

/**
 * Check if thread is running
 */
bool output_publisher_is_running(const output_publisher_t* publisher);

/**
 * Get statistics
 */
uint64_t output_publisher_get_messages_published(const output_publisher_t* publisher);

/* ============================================================================
 * Internal Functions (used by thread)
 * ============================================================================ */

/**
 * Thread entry point
 */
void* output_publisher_thread_func(void* arg);

/**
 * Publish a single output message to stdout
 */
void output_publisher_publish_message(output_publisher_t* publisher, const output_msg_t* msg);

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_OUTPUT_PUBLISHER_H */

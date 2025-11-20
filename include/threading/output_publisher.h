#ifndef OUTPUT_PUBLISHER_H
#define OUTPUT_PUBLISHER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "message_types_extended.h"
#include "lockfree_queue.h"

/**
 * Output Publisher Thread (UDP Mode Only)
 * 
 * In UDP mode, this thread consumes output envelopes and writes formatted
 * messages to stdout. In TCP mode, the output_router handles distribution
 * to individual clients instead.
 * 
 * Flow (UDP):
 *   Processor → Output Queue (envelopes) → Output Publisher → stdout
 */

// Use the envelope queue (same as TCP)
DECLARE_LOCKFREE_QUEUE(output_msg_envelope_t, output_envelope_queue)

/**
 * Output publisher configuration
 */
typedef struct {
    bool use_binary_output;             // Binary vs CSV output format
} output_publisher_config_t;

/**
 * Output publisher context
 */
typedef struct {
    // Configuration
    output_publisher_config_t config;
    
    // Input queue (from processor)
    output_envelope_queue_t* input_queue;
    
    // Shutdown coordination
    atomic_bool* shutdown_flag;
    
    // Statistics
    uint64_t messages_published;
    
} output_publisher_context_t;

/**
 * Initialize output publisher context
 */
bool output_publisher_init(output_publisher_context_t* ctx,
                           const output_publisher_config_t* config,
                           output_envelope_queue_t* input_queue,
                           atomic_bool* shutdown_flag);

/**
 * Cleanup output publisher context
 */
void output_publisher_cleanup(output_publisher_context_t* ctx);

/**
 * Output publisher thread entry point
 */
void* output_publisher_thread(void* arg);

/**
 * Print output publisher statistics
 */
void output_publisher_print_stats(const output_publisher_context_t* ctx);

#endif // OUTPUT_PUBLISHER_H

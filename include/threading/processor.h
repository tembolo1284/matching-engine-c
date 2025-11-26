#ifndef MATCHING_ENGINE_PROCESSOR_H
#define MATCHING_ENGINE_PROCESSOR_H

#include "protocol/message_types_extended.h"
#include "core/matching_engine.h"
#include "threading/queues.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Processor - Worker thread for order processing
 *
 * Power of Ten Compliant:
 * - Rule 2: All loops bounded
 * - Rule 3: No dynamic allocation
 * - Rule 5: Assertions for invariants
 *
 * Performance Optimizations:
 * - Batch processing (32 messages at a time)
 * - Batched statistics updates (not per-message)
 * - Configurable spin-wait vs sleep
 * - Output buffer reused across batch
 * - Prefetching hints for next message
 *
 * TCP multi-client support:
 * - Envelope types for client routing
 * - Trade messages routed to both participants
 * - Client disconnection triggers order cancellation
 */

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define PROCESSOR_BATCH_SIZE 32
#define PROCESSOR_SLEEP_NS 1000      /* 1 microsecond */
#define PROCESSOR_SPIN_ITERATIONS 100 /* Spin this many times before sleeping */

/* Prefetch hint for next cache line (if supported) */
#if defined(__GNUC__) || defined(__clang__)
    #define PREFETCH_READ(addr) __builtin_prefetch((addr), 0, 3)
    #define PREFETCH_WRITE(addr) __builtin_prefetch((addr), 1, 3)
#else
    #define PREFETCH_READ(addr) ((void)0)
    #define PREFETCH_WRITE(addr) ((void)0)
#endif

/* ============================================================================
 * Structures
 * ============================================================================ */

/**
 * Processor configuration
 */
typedef struct {
    bool tcp_mode;          /* true = TCP (client routing), false = UDP */
    bool spin_wait;         /* true = busy-wait, false = nanosleep when idle */
    int processor_id;       /* For dual-processor mode (0 or 1) */
} processor_config_t;

/**
 * Processor statistics - cache-line aligned to prevent false sharing
 * Updated in batches, not per-message, for better performance
 * 
 * Layout (64 bytes on most platforms):
 *   0-7:   messages_processed
 *   8-15:  batches_processed
 *   16-23: output_messages
 *   24-31: trades_processed
 *   32-39: empty_polls
 *   40-47: output_queue_full
 *   48-63: padding
 */
#if defined(__GNUC__) || defined(__clang__)
typedef struct __attribute__((aligned(64))) {
#else
typedef struct {
#endif
    uint64_t messages_processed;     /* Not atomic - updated only by owning thread */
    uint64_t batches_processed;
    uint64_t output_messages;
    uint64_t trades_processed;
    uint64_t empty_polls;            /* Times we polled with no messages */
    uint64_t output_queue_full;      /* Times output queue was full */
    uint8_t _pad[16];                /* Pad to 64 bytes */
} processor_stats_t;

_Static_assert(sizeof(processor_stats_t) == 64, "processor_stats_t should be cache-line sized");

/**
 * Processor state
 */
typedef struct {
    /* Configuration - read-only after init */
    processor_config_t config;
    
    /* Queues */
    input_envelope_queue_t* input_queue;
    output_envelope_queue_t* output_queue;
    
    /* Matching engine */
    matching_engine_t* engine;
    
    /* Thread management */
    pthread_t thread;
    atomic_bool running;
    atomic_bool started;
    atomic_bool* shutdown_flag;
    
    /* Sequence counter for output messages */
    atomic_uint_fast64_t output_sequence;
    
    /* Statistics - on separate cache line */
    processor_stats_t stats;
    
} processor_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize processor
 * 
 * @param processor Processor to initialize
 * @param config Configuration (copied)
 * @param engine Matching engine (must outlive processor)
 * @param input_queue Input queue (must outlive processor)
 * @param output_queue Output queue (must outlive processor)
 * @param shutdown_flag Shared shutdown flag
 * @return true on success
 */
bool processor_init(processor_t* processor,
                    const processor_config_t* config,
                    matching_engine_t* engine,
                    input_envelope_queue_t* input_queue,
                    output_envelope_queue_t* output_queue,
                    atomic_bool* shutdown_flag);

/**
 * Cleanup processor resources
 */
void processor_cleanup(processor_t* processor);

/**
 * Thread entry point - do not call directly, use pthread_create
 */
void* processor_thread(void* arg);

/**
 * Print statistics to stderr
 */
void processor_print_stats(const processor_t* processor);

/**
 * Cancel all orders for a specific client (TCP mode)
 * Thread-safe: can be called from listener thread
 */
void processor_cancel_client_orders(processor_t* processor, uint32_t client_id);

/**
 * Get current statistics snapshot
 * Note: Stats may be slightly stale due to batched updates
 */
static inline void processor_get_stats(const processor_t* processor,
                                       uint64_t* messages,
                                       uint64_t* batches,
                                       uint64_t* outputs,
                                       uint64_t* trades) {
    if (messages) *messages = processor->stats.messages_processed;
    if (batches) *batches = processor->stats.batches_processed;
    if (outputs) *outputs = processor->stats.output_messages;
    if (trades) *trades = processor->stats.trades_processed;
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_PROCESSOR_H */

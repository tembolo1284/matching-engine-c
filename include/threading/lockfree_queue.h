#ifndef MATCHING_ENGINE_LOCKFREE_QUEUE_H
#define MATCHING_ENGINE_LOCKFREE_QUEUE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdalign.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * LockFreeQueue - Single-producer, single-consumer lock-free queue
 *
 * Design decisions:
 * - Fixed-size ring buffer (power of 2 for efficient modulo via bitmasking)
 * - Cache-line padding to prevent false sharing between producer/consumer
 * - Lock-free using C11 atomic operations
 * - Template-like functionality via macros
 * - CRITICAL: Stats are NOT atomic - updated only by owning thread
 *
 * Power of Ten Compliance:
 * - Rule 2: All loops bounded by queue size
 * - Rule 3: No dynamic allocation (fixed-size buffer)
 * - Rule 5: Assertions verify invariants (minimum 2 per function)
 * - Rule 7: All return values checked
 *
 * Performance Optimizations:
 * - Producer stats on producer's cache line (no sharing)
 * - Consumer stats on consumer's cache line (no sharing)
 * - No atomic operations for statistics (single writer per stat group)
 * - Batch dequeue to amortize atomic operations
 * - No CAS loops on hot path
 */

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Cache line size (typically 64 bytes on modern CPUs) */
#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

/* Default queue size (must be power of 2) */
#ifndef LOCKFREE_QUEUE_SIZE
#define LOCKFREE_QUEUE_SIZE 65536
#endif

/* Index mask for fast modulo */
#define LOCKFREE_QUEUE_MASK (LOCKFREE_QUEUE_SIZE - 1)

/* Maximum reasonable queue size for safety */
#define MAX_QUEUE_SIZE 2097152  /* 2M entries */

/* Compile-time checks for queue size */
_Static_assert((LOCKFREE_QUEUE_SIZE & (LOCKFREE_QUEUE_SIZE - 1)) == 0,
               "LOCKFREE_QUEUE_SIZE must be power of 2");
_Static_assert(LOCKFREE_QUEUE_SIZE >= 16,
               "LOCKFREE_QUEUE_SIZE too small (min 16)");
_Static_assert(LOCKFREE_QUEUE_SIZE <= MAX_QUEUE_SIZE,
               "LOCKFREE_QUEUE_SIZE too large");

/* ============================================================================
 * Queue Declaration Macro
 * ============================================================================
 *
 * Usage:
 *   DECLARE_LOCKFREE_QUEUE(input_msg_t, input_queue)
 *
 * Memory Layout (optimized for false-sharing prevention):
 *   Cache Line 0: head index (consumer-owned)
 *   Cache Line 1: tail index (producer-owned)  
 *   Cache Line 2: producer stats (producer-owned, NOT atomic)
 *   Cache Line 3: consumer stats (consumer-owned, NOT atomic)
 *   Cache Line 4+: ring buffer (aligned)
 *
 * CRITICAL: Stats are plain integers, not atomics.
 * - Producer stats updated ONLY by producer thread
 * - Consumer stats updated ONLY by consumer thread
 * - Reading stats from another thread may see stale values (acceptable)
 */
#define DECLARE_LOCKFREE_QUEUE(TYPE, NAME) \
    \
    /* Producer statistics - updated ONLY by producer, no atomics needed */ \
    typedef struct { \
        size_t total_enqueues;      /* Successful enqueues */ \
        size_t failed_enqueues;     /* Queue-full events */ \
        size_t peak_size;           /* High water mark */ \
    } NAME##_producer_stats_t; \
    \
    /* Consumer statistics - updated ONLY by consumer, no atomics needed */ \
    typedef struct { \
        size_t total_dequeues;      /* Successful dequeues */ \
        size_t batch_dequeues;      /* Batch dequeue operations */ \
    } NAME##_consumer_stats_t; \
    \
    typedef struct NAME##_t { \
        /* Head index (consumer side) - own cache line */ \
        _Alignas(CACHE_LINE_SIZE) atomic_size_t head; \
        uint8_t _pad_head[CACHE_LINE_SIZE - sizeof(atomic_size_t)]; \
        \
        /* Tail index (producer side) - own cache line */ \
        _Alignas(CACHE_LINE_SIZE) atomic_size_t tail; \
        uint8_t _pad_tail[CACHE_LINE_SIZE - sizeof(atomic_size_t)]; \
        \
        /* Producer stats - own cache line, NOT atomic */ \
        _Alignas(CACHE_LINE_SIZE) NAME##_producer_stats_t producer_stats; \
        uint8_t _pad_prod[CACHE_LINE_SIZE - sizeof(NAME##_producer_stats_t)]; \
        \
        /* Consumer stats - own cache line, NOT atomic */ \
        _Alignas(CACHE_LINE_SIZE) NAME##_consumer_stats_t consumer_stats; \
        uint8_t _pad_cons[CACHE_LINE_SIZE - sizeof(NAME##_consumer_stats_t)]; \
        \
        /* Ring buffer storage - cache-line aligned */ \
        _Alignas(CACHE_LINE_SIZE) TYPE buffer[LOCKFREE_QUEUE_SIZE]; \
    } NAME##_t; \
    \
    /* Compile-time checks for cache line separation */ \
    _Static_assert(offsetof(NAME##_t, tail) >= CACHE_LINE_SIZE, \
                   #NAME "_t: head and tail must be on separate cache lines"); \
    _Static_assert(offsetof(NAME##_t, producer_stats) >= 2 * CACHE_LINE_SIZE, \
                   #NAME "_t: producer_stats must be on its own cache line"); \
    _Static_assert(offsetof(NAME##_t, consumer_stats) >= 3 * CACHE_LINE_SIZE, \
                   #NAME "_t: consumer_stats must be on its own cache line"); \
    \
    /* Function declarations */ \
    void NAME##_init(NAME##_t* queue); \
    void NAME##_destroy(NAME##_t* queue); \
    bool NAME##_enqueue(NAME##_t* queue, const TYPE* item); \
    bool NAME##_dequeue(NAME##_t* queue, TYPE* item); \
    size_t NAME##_dequeue_batch(NAME##_t* queue, TYPE* items, size_t max_items); \
    bool NAME##_empty(const NAME##_t* queue); \
    size_t NAME##_size(const NAME##_t* queue); \
    size_t NAME##_capacity(const NAME##_t* queue); \
    bool NAME##_verify_invariants(const NAME##_t* queue); \
    void NAME##_get_stats(const NAME##_t* queue, \
                          size_t* total_enq, size_t* total_deq, \
                          size_t* failed_enq, size_t* peak);

/* ============================================================================
 * Queue Implementation Macro
 * ============================================================================
 *
 * Usage (in .c file):
 *   DEFINE_LOCKFREE_QUEUE(input_msg_t, input_queue)
 */
#define DEFINE_LOCKFREE_QUEUE(TYPE, NAME) \
    \
    /* Initialize queue */ \
    void NAME##_init(NAME##_t* queue) { \
        /* Rule 5: Preconditions */ \
        assert(queue != NULL && "NULL queue in " #NAME "_init"); \
        \
        /* Initialize indices */ \
        atomic_init(&queue->head, 0); \
        atomic_init(&queue->tail, 0); \
        \
        /* Initialize producer stats (plain integers) */ \
        queue->producer_stats.total_enqueues = 0; \
        queue->producer_stats.failed_enqueues = 0; \
        queue->producer_stats.peak_size = 0; \
        \
        /* Initialize consumer stats (plain integers) */ \
        queue->consumer_stats.total_dequeues = 0; \
        queue->consumer_stats.batch_dequeues = 0; \
        \
        /* Rule 5: Postcondition */ \
        assert(atomic_load(&queue->head) == 0 && "head not zero after init"); \
        assert(atomic_load(&queue->tail) == 0 && "tail not zero after init"); \
    } \
    \
    /* Destroy queue (no-op for fixed array) */ \
    void NAME##_destroy(NAME##_t* queue) { \
        assert(queue != NULL && "NULL queue in " #NAME "_destroy"); \
        /* Rule 5: Verify queue is empty on destroy (catches leaks) */ \
        assert(NAME##_empty(queue) && "Queue not empty on destroy - potential leak"); \
        (void)queue; \
    } \
    \
    /** \
     * Enqueue element (producer only) \
     * \
     * Performance: Single atomic load + single atomic store \
     * Stats: Plain increment (no atomic) - producer owns these \
     */ \
    bool NAME##_enqueue(NAME##_t* queue, const TYPE* item) { \
        /* Rule 5: Preconditions */ \
        assert(queue != NULL && "NULL queue in " #NAME "_enqueue"); \
        assert(item != NULL && "NULL item in " #NAME "_enqueue"); \
        \
        const size_t current_tail = atomic_load_explicit(&queue->tail, memory_order_relaxed); \
        const size_t next_tail = (current_tail + 1) & LOCKFREE_QUEUE_MASK; \
        \
        /* Check if full - acquire to see consumer's progress */ \
        const size_t current_head = atomic_load_explicit(&queue->head, memory_order_acquire); \
        if (next_tail == current_head) { \
            queue->producer_stats.failed_enqueues++; \
            return false; \
        } \
        \
        /* Store item */ \
        queue->buffer[current_tail] = *item; \
        \
        /* Publish tail - release so consumer sees the item */ \
        atomic_store_explicit(&queue->tail, next_tail, memory_order_release); \
        \
        /* Update producer stats (no atomic - single writer) */ \
        queue->producer_stats.total_enqueues++; \
        \
        /* Update peak size (no CAS - just a simple compare) */ \
        size_t current_size = (next_tail - current_head) & LOCKFREE_QUEUE_MASK; \
        if (current_size > queue->producer_stats.peak_size) { \
            queue->producer_stats.peak_size = current_size; \
        } \
        \
        return true; \
    } \
    \
    /** \
     * Dequeue single element (consumer only) \
     * \
     * Performance: Single atomic load + single atomic store \
     * Stats: Plain increment (no atomic) - consumer owns these \
     * \
     * NOTE: Empty queue is NOT counted as failure - it's normal operation \
     */ \
    bool NAME##_dequeue(NAME##_t* queue, TYPE* item) { \
        /* Rule 5: Preconditions */ \
        assert(queue != NULL && "NULL queue in " #NAME "_dequeue"); \
        assert(item != NULL && "NULL item in " #NAME "_dequeue"); \
        \
        const size_t current_head = atomic_load_explicit(&queue->head, memory_order_relaxed); \
        \
        /* Check if empty - acquire to see producer's writes */ \
        const size_t current_tail = atomic_load_explicit(&queue->tail, memory_order_acquire); \
        if (current_head == current_tail) { \
            /* Empty queue is normal, not a failure - don't increment any counter */ \
            return false; \
        } \
        \
        /* Load item */ \
        *item = queue->buffer[current_head]; \
        \
        /* Advance head - release so producer can reuse slot */ \
        atomic_store_explicit(&queue->head, \
                             (current_head + 1) & LOCKFREE_QUEUE_MASK, \
                             memory_order_release); \
        \
        /* Update consumer stats (no atomic - single writer) */ \
        queue->consumer_stats.total_dequeues++; \
        \
        return true; \
    } \
    \
    /** \
     * Batch dequeue - dequeue up to max_items in one operation \
     * \
     * Performance: Single atomic load + single atomic store for entire batch \
     * This amortizes the atomic overhead across multiple items. \
     * \
     * @param queue     Queue to dequeue from \
     * @param items     Output array (must have space for max_items) \
     * @param max_items Maximum items to dequeue \
     * @return          Number of items actually dequeued (0 to max_items) \
     */ \
    size_t NAME##_dequeue_batch(NAME##_t* queue, TYPE* items, size_t max_items) { \
        /* Rule 5: Preconditions */ \
        assert(queue != NULL && "NULL queue in " #NAME "_dequeue_batch"); \
        assert(items != NULL && "NULL items in " #NAME "_dequeue_batch"); \
        \
        if (max_items == 0) { \
            return 0; \
        } \
        \
        const size_t head = atomic_load_explicit(&queue->head, memory_order_relaxed); \
        const size_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire); \
        \
        /* Calculate available items */ \
        size_t available = (tail - head) & LOCKFREE_QUEUE_MASK; \
        size_t to_dequeue = (available < max_items) ? available : max_items; \
        \
        if (to_dequeue == 0) { \
            return 0; \
        } \
        \
        /* Copy items - Rule 2: bounded by to_dequeue <= max_items */ \
        for (size_t i = 0; i < to_dequeue; i++) { \
            items[i] = queue->buffer[(head + i) & LOCKFREE_QUEUE_MASK]; \
        } \
        \
        /* Single atomic store for entire batch */ \
        atomic_store_explicit(&queue->head, \
                             (head + to_dequeue) & LOCKFREE_QUEUE_MASK, \
                             memory_order_release); \
        \
        /* Update consumer stats (no atomic - single writer) */ \
        queue->consumer_stats.total_dequeues += to_dequeue; \
        queue->consumer_stats.batch_dequeues++; \
        \
        return to_dequeue; \
    } \
    \
    /** \
     * Check if queue is empty \
     * \
     * NOTE: Result may be stale immediately after return due to concurrency. \
     * Use only for monitoring/debugging, not for synchronization. \
     */ \
    bool NAME##_empty(const NAME##_t* queue) { \
        assert(queue != NULL && "NULL queue in " #NAME "_empty"); \
        assert(atomic_load(&queue->head) < LOCKFREE_QUEUE_SIZE && "head out of bounds"); \
        \
        return atomic_load_explicit(&queue->head, memory_order_acquire) == \
               atomic_load_explicit(&queue->tail, memory_order_acquire); \
    } \
    \
    /** \
     * Get approximate queue size \
     * \
     * NOTE: Result may be stale immediately after return due to concurrency. \
     */ \
    size_t NAME##_size(const NAME##_t* queue) { \
        assert(queue != NULL && "NULL queue in " #NAME "_size"); \
        assert(atomic_load(&queue->tail) < LOCKFREE_QUEUE_SIZE && "tail out of bounds"); \
        \
        const size_t head = atomic_load_explicit(&queue->head, memory_order_acquire); \
        const size_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire); \
        \
        return (tail - head) & LOCKFREE_QUEUE_MASK; \
    } \
    \
    /** \
     * Get queue capacity (always LOCKFREE_QUEUE_SIZE - 1) \
     */ \
    size_t NAME##_capacity(const NAME##_t* queue) { \
        assert(queue != NULL && "NULL queue in " #NAME "_capacity"); \
        (void)queue; \
        return LOCKFREE_QUEUE_SIZE - 1; /* One slot reserved for full detection */ \
    } \
    \
    /** \
     * Verify queue invariants (for testing/debugging) \
     */ \
    bool NAME##_verify_invariants(const NAME##_t* queue) { \
        assert(queue != NULL && "NULL queue in " #NAME "_verify_invariants"); \
        \
        size_t head = atomic_load(&queue->head); \
        size_t tail = atomic_load(&queue->tail); \
        \
        /* Check indices are within bounds */ \
        if (head >= LOCKFREE_QUEUE_SIZE) { \
            fprintf(stderr, "[" #NAME "] head out of bounds: %zu\n", head); \
            return false; \
        } \
        if (tail >= LOCKFREE_QUEUE_SIZE) { \
            fprintf(stderr, "[" #NAME "] tail out of bounds: %zu\n", tail); \
            return false; \
        } \
        \
        /* Check size calculation doesn't overflow */ \
        size_t size = (tail - head) & LOCKFREE_QUEUE_MASK; \
        if (size >= LOCKFREE_QUEUE_SIZE) { \
            fprintf(stderr, "[" #NAME "] size calculation error: %zu\n", size); \
            return false; \
        } \
        \
        /* Check stats are consistent */ \
        if (queue->producer_stats.total_enqueues < queue->consumer_stats.total_dequeues) { \
            fprintf(stderr, "[" #NAME "] dequeues > enqueues (impossible)\n"); \
            return false; \
        } \
        \
        return true; \
    } \
    \
    /** \
     * Get statistics snapshot \
     * \
     * NOTE: Stats may be slightly stale due to non-atomic reads from other thread. \
     * This is acceptable for monitoring - exact values not required. \
     */ \
    void NAME##_get_stats(const NAME##_t* queue, \
                         size_t* total_enq, size_t* total_deq, \
                         size_t* failed_enq, size_t* peak) { \
        assert(queue != NULL && "NULL queue in " #NAME "_get_stats"); \
        \
        /* Read producer stats (may be stale if called from consumer thread) */ \
        if (total_enq) *total_enq = queue->producer_stats.total_enqueues; \
        if (failed_enq) *failed_enq = queue->producer_stats.failed_enqueues; \
        if (peak) *peak = queue->producer_stats.peak_size; \
        \
        /* Read consumer stats (may be stale if called from producer thread) */ \
        if (total_deq) *total_deq = queue->consumer_stats.total_dequeues; \
    }

/* Legacy alias for backward compatibility */
#define IMPLEMENT_LOCKFREE_QUEUE(TYPE, NAME) DEFINE_LOCKFREE_QUEUE(TYPE, NAME)

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_LOCKFREE_QUEUE_H */

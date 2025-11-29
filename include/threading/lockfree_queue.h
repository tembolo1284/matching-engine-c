#ifndef MATCHING_ENGINE_LOCKFREE_QUEUE_H
#define MATCHING_ENGINE_LOCKFREE_QUEUE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * LockFreeQueue - Single-producer, single-consumer lock-free queue
 *
 * Design decisions:
 * - Fixed-size ring buffer (power of 2 for efficient modulo via bitmasking)
 * - Cache-line padding to prevent false sharing
 * - Lock-free using C11 atomic operations
 * - Template-like functionality via macros
 *
 * Power of Ten Compliance:
 * - Rule 2: All loops bounded by queue size
 * - Rule 3: No dynamic allocation (fixed-size buffer)
 * - Rule 5: Assertions verify invariants
 * - Rule 7: All return values checked
 *
 * CRITICAL: Default size 16384 to handle UDP bursts
 */

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Cache line size (typically 64 bytes on modern CPUs) */
#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

/* Default queue size (must be power of 2) - configurable */
#ifndef LOCKFREE_QUEUE_SIZE
#define LOCKFREE_QUEUE_SIZE 65536
#endif

/* Index mask for fast modulo */
#define LOCKFREE_QUEUE_MASK (LOCKFREE_QUEUE_SIZE - 1)

/* Maximum reasonable queue size for safety */
#define MAX_QUEUE_SIZE 2097152  /* 1M entries */

/* Compile-time checks for queue size */
_Static_assert((LOCKFREE_QUEUE_SIZE & (LOCKFREE_QUEUE_SIZE - 1)) == 0,
               "LOCKFREE_QUEUE_SIZE must be power of 2");
_Static_assert(LOCKFREE_QUEUE_SIZE >= 16,
               "LOCKFREE_QUEUE_SIZE too small (min 16)");
_Static_assert(LOCKFREE_QUEUE_SIZE <= MAX_QUEUE_SIZE,
               "LOCKFREE_QUEUE_SIZE too large (max 1M)");

/* ============================================================================
 * Debug Assertions
 * ============================================================================ */

#ifdef DEBUG
  #define QUEUE_ASSERT(expr) \
    do { \
      if (!(expr)) { \
        fprintf(stderr, "%s:%d: Queue assertion '%s' failed\n", \
                __FILE__, __LINE__, #expr); \
        return false; \
      } \
    } while(0)

  #define QUEUE_ASSERT_VOID(expr) \
    do { \
      if (!(expr)) { \
        fprintf(stderr, "%s:%d: Queue assertion '%s' failed\n", \
                __FILE__, __LINE__, #expr); \
        return; \
      } \
    } while(0)
#else
  #define QUEUE_ASSERT(expr) ((void)0)
  #define QUEUE_ASSERT_VOID(expr) ((void)0)
#endif

/* ============================================================================
 * Queue Declaration Macro
 * ============================================================================
 *
 * Usage:
 *   DECLARE_LOCKFREE_QUEUE(input_msg_t, input_queue)
 *
 * Generates:
 *   - struct input_queue_t { ... }
 *   - Function declarations for init/destroy/enqueue/dequeue/etc.
 *
 * Memory Layout (assuming 64-byte cache lines):
 *   Offset 0:   head (8 bytes) + padding (56 bytes) = 64 bytes
 *   Offset 64:  tail (8 bytes) + padding (56 bytes) = 64 bytes
 *   Offset 128: stats (40 bytes) + padding (24 bytes) = 64 bytes
 *   Offset 192: buffer[LOCKFREE_QUEUE_SIZE] - cache-line aligned
 */
#define DECLARE_LOCKFREE_QUEUE(TYPE, NAME) \
    typedef struct NAME##_t { \
        /* Head index (consumer side) - own cache line */ \
        _Alignas(CACHE_LINE_SIZE) atomic_size_t head; \
        uint8_t _pad_head[CACHE_LINE_SIZE - sizeof(atomic_size_t)]; \
        \
        /* Tail index (producer side) - own cache line */ \
        _Alignas(CACHE_LINE_SIZE) atomic_size_t tail; \
        uint8_t _pad_tail[CACHE_LINE_SIZE - sizeof(atomic_size_t)]; \
        \
        /* Statistics - own cache line */ \
        _Alignas(CACHE_LINE_SIZE) struct { \
            atomic_size_t total_enqueues; \
            atomic_size_t total_dequeues; \
            atomic_size_t failed_enqueues; \
            atomic_size_t failed_dequeues; \
            atomic_size_t peak_size; \
        } stats; \
        uint8_t _pad_stats[CACHE_LINE_SIZE - 5 * sizeof(atomic_size_t)]; \
        \
        /* Ring buffer storage - cache-line aligned */ \
        _Alignas(CACHE_LINE_SIZE) TYPE buffer[LOCKFREE_QUEUE_SIZE]; \
    } NAME##_t; \
    \
    /* Compile-time check: head and tail on separate cache lines */ \
    _Static_assert(offsetof(NAME##_t, tail) >= CACHE_LINE_SIZE, \
                   #NAME "_t: head and tail must be on separate cache lines"); \
    \
    /* Function declarations */ \
    void NAME##_init(NAME##_t* queue); \
    void NAME##_destroy(NAME##_t* queue); \
    bool NAME##_enqueue(NAME##_t* queue, const TYPE* item); \
    bool NAME##_dequeue(NAME##_t* queue, TYPE* item); \
    bool NAME##_empty(const NAME##_t* queue); \
    size_t NAME##_size(const NAME##_t* queue); \
    size_t NAME##_capacity(const NAME##_t* queue); \
    bool NAME##_verify_invariants(const NAME##_t* queue); \
    void NAME##_get_stats(const NAME##_t* queue, \
                          size_t* total_enq, size_t* total_deq, \
                          size_t* failed_enq, size_t* failed_deq, \
                          size_t* peak);

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
        if (queue == NULL) { \
            return; \
        } \
        \
        /* Initialize indices */ \
        atomic_init(&queue->head, 0); \
        atomic_init(&queue->tail, 0); \
        \
        /* Initialize stats */ \
        atomic_init(&queue->stats.total_enqueues, 0); \
        atomic_init(&queue->stats.total_dequeues, 0); \
        atomic_init(&queue->stats.failed_enqueues, 0); \
        atomic_init(&queue->stats.failed_dequeues, 0); \
        atomic_init(&queue->stats.peak_size, 0); \
    } \
    \
    /* Destroy queue (no-op for fixed array, provided for API consistency) */ \
    void NAME##_destroy(NAME##_t* queue) { \
        (void)queue; \
    } \
    \
    /* Enqueue element (returns false if queue is full) */ \
    bool NAME##_enqueue(NAME##_t* queue, const TYPE* item) { \
        if (queue == NULL || item == NULL) { \
            return false; \
        } \
        \
        const size_t current_tail = atomic_load_explicit(&queue->tail, memory_order_relaxed); \
        const size_t next_tail = (current_tail + 1) & LOCKFREE_QUEUE_MASK; \
        \
        /* Check if full */ \
        if (next_tail == atomic_load_explicit(&queue->head, memory_order_acquire)) { \
            atomic_fetch_add(&queue->stats.failed_enqueues, 1); \
            return false; \
        } \
        \
        /* Store item and advance tail */ \
        queue->buffer[current_tail] = *item; \
        atomic_store_explicit(&queue->tail, next_tail, memory_order_release); \
        \
        /* Update stats */ \
        atomic_fetch_add(&queue->stats.total_enqueues, 1); \
        \
        /* Update peak size (relaxed - approximate is fine) */ \
        size_t current_size = (next_tail - atomic_load_explicit(&queue->head, memory_order_relaxed)) \
                              & LOCKFREE_QUEUE_MASK; \
        size_t old_peak = atomic_load_explicit(&queue->stats.peak_size, memory_order_relaxed); \
        while (current_size > old_peak) { \
            if (atomic_compare_exchange_weak_explicit(&queue->stats.peak_size, \
                    &old_peak, current_size, \
                    memory_order_relaxed, memory_order_relaxed)) { \
                break; \
            } \
        } \
        \
        return true; \
    } \
    \
    /* Dequeue element (returns false if queue is empty) */ \
    bool NAME##_dequeue(NAME##_t* queue, TYPE* item) { \
        if (queue == NULL || item == NULL) { \
            return false; \
        } \
        \
        const size_t current_head = atomic_load_explicit(&queue->head, memory_order_relaxed); \
        \
        /* Check if empty */ \
        if (current_head == atomic_load_explicit(&queue->tail, memory_order_acquire)) { \
            atomic_fetch_add(&queue->stats.failed_dequeues, 1); \
            return false; \
        } \
        \
        /* Load item and advance head */ \
        *item = queue->buffer[current_head]; \
        atomic_store_explicit(&queue->head, \
                             (current_head + 1) & LOCKFREE_QUEUE_MASK, \
                             memory_order_release); \
        \
        /* Update stats */ \
        atomic_fetch_add(&queue->stats.total_dequeues, 1); \
        \
        return true; \
    } \
    \
    /* Check if queue is empty */ \
    bool NAME##_empty(const NAME##_t* queue) { \
        if (queue == NULL) { \
            return true; \
        } \
        \
        return atomic_load_explicit(&queue->head, memory_order_acquire) == \
               atomic_load_explicit(&queue->tail, memory_order_acquire); \
    } \
    \
    /* Get approximate size (may be stale due to concurrent access) */ \
    size_t NAME##_size(const NAME##_t* queue) { \
        if (queue == NULL) { \
            return 0; \
        } \
        \
        const size_t head = atomic_load_explicit(&queue->head, memory_order_acquire); \
        const size_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire); \
        \
        return (tail - head) & LOCKFREE_QUEUE_MASK; \
    } \
    \
    /* Get capacity */ \
    size_t NAME##_capacity(const NAME##_t* queue) { \
        (void)queue; \
        return LOCKFREE_QUEUE_SIZE - 1; /* One slot reserved for full detection */ \
    } \
    \
    /* Verify queue invariants (for testing/debugging) */ \
    bool NAME##_verify_invariants(const NAME##_t* queue) { \
        if (queue == NULL) { \
            return false; \
        } \
        \
        size_t head = atomic_load(&queue->head); \
        size_t tail = atomic_load(&queue->tail); \
        \
        /* Check indices are within bounds */ \
        if (head >= LOCKFREE_QUEUE_SIZE || tail >= LOCKFREE_QUEUE_SIZE) { \
            fprintf(stderr, "Queue indices out of bounds: head=%zu, tail=%zu\n", \
                   head, tail); \
            return false; \
        } \
        \
        /* Check size calculation doesn't overflow */ \
        size_t size = (tail - head) & LOCKFREE_QUEUE_MASK; \
        if (size >= LOCKFREE_QUEUE_SIZE) { \
            fprintf(stderr, "Queue size calculation error: size=%zu\n", size); \
            return false; \
        } \
        \
        return true; \
    } \
    \
    /* Get statistics (for monitoring/debugging) */ \
    void NAME##_get_stats(const NAME##_t* queue, \
                         size_t* total_enq, size_t* total_deq, \
                         size_t* failed_enq, size_t* failed_deq, \
                         size_t* peak) { \
        if (queue == NULL) { \
            return; \
        } \
        \
        if (total_enq) *total_enq = atomic_load(&queue->stats.total_enqueues); \
        if (total_deq) *total_deq = atomic_load(&queue->stats.total_dequeues); \
        if (failed_enq) *failed_enq = atomic_load(&queue->stats.failed_enqueues); \
        if (failed_deq) *failed_deq = atomic_load(&queue->stats.failed_dequeues); \
        if (peak) *peak = atomic_load(&queue->stats.peak_size); \
    }

/* Legacy alias for backward compatibility */
#define IMPLEMENT_LOCKFREE_QUEUE(TYPE, NAME) DEFINE_LOCKFREE_QUEUE(TYPE, NAME)

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_LOCKFREE_QUEUE_H */

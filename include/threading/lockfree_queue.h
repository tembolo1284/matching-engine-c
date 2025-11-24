#ifndef MATCHING_ENGINE_LOCKFREE_QUEUE_H
#define MATCHING_ENGINE_LOCKFREE_QUEUE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>

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
 * CRITICAL: Default size 16384 to handle UDP bursts
 */

/* Cache line size (typically 64 bytes on modern CPUs) */
#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

/* Default queue size (must be power of 2) - configurable */
#ifndef LOCKFREE_QUEUE_SIZE
#define LOCKFREE_QUEUE_SIZE 16384
#endif

/* Maximum reasonable queue size for safety */
#define MAX_QUEUE_SIZE 1048576  /* 1M entries */

/* Compile-time checks for queue size */
_Static_assert((LOCKFREE_QUEUE_SIZE & (LOCKFREE_QUEUE_SIZE - 1)) == 0, 
               "LOCKFREE_QUEUE_SIZE must be power of 2");
_Static_assert(LOCKFREE_QUEUE_SIZE >= 16, 
               "LOCKFREE_QUEUE_SIZE too small (min 16)");
_Static_assert(LOCKFREE_QUEUE_SIZE <= MAX_QUEUE_SIZE,
               "LOCKFREE_QUEUE_SIZE too large (max 1M)");

/* Padding macro to prevent false sharing */
#define CACHE_LINE_PAD(n) char _pad##n[CACHE_LINE_SIZE]

/* Rule 5: Assertion macro with error recovery */
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

/**
 * Macro to declare a lock-free queue type
 *
 * Usage:
 *   DECLARE_LOCKFREE_QUEUE(input_msg_t, input_queue)
 *
 * Generates:
 *   - struct input_queue_t { ... }
 *   - Functions for init/destroy/enqueue/dequeue/empty/size/verify
 */
#define DECLARE_LOCKFREE_QUEUE(TYPE, NAME) \
    typedef struct NAME##_t { \
        /* Head index (consumer side) - aligned to cache line */ \
        _Alignas(CACHE_LINE_SIZE) atomic_size_t head; \
        \
        /* Padding to prevent false sharing between head and tail */ \
        CACHE_LINE_PAD(1); \
        \
        /* Tail index (producer side) - aligned to cache line */ \
        _Alignas(CACHE_LINE_SIZE) atomic_size_t tail; \
        \
        /* Padding after tail */ \
        CACHE_LINE_PAD(2); \
        \
        /* Debug/monitoring fields (on separate cache line) */ \
        _Alignas(CACHE_LINE_SIZE) struct { \
            atomic_size_t total_enqueues; \
            atomic_size_t total_dequeues; \
            atomic_size_t failed_enqueues; \
            atomic_size_t failed_dequeues; \
            size_t peak_size; \
        } stats; \
        \
        /* Padding after stats */ \
        CACHE_LINE_PAD(3); \
        \
        /* Ring buffer storage */ \
        TYPE buffer[LOCKFREE_QUEUE_SIZE]; \
        \
        /* Configuration */ \
        size_t capacity; \
        size_t index_mask; /* For fast modulo */ \
    } NAME##_t; \
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

/**
 * Macro to implement lock-free queue functions
 *
 * Usage (in .c file):
 *   DEFINE_LOCKFREE_QUEUE(input_msg_t, input_queue)
 */
#define DEFINE_LOCKFREE_QUEUE(TYPE, NAME) \
    \
    /* Initialize queue */ \
    void NAME##_init(NAME##_t* queue) { \
        /* Rule 7: Parameter validation */ \
        QUEUE_ASSERT_VOID(queue != NULL); \
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
        queue->stats.peak_size = 0; \
        \
        /* Set configuration */ \
        queue->capacity = LOCKFREE_QUEUE_SIZE; \
        queue->index_mask = LOCKFREE_QUEUE_SIZE - 1; \
        \
        /* Rule 5: Verify post-conditions */ \
        QUEUE_ASSERT_VOID(queue->capacity == LOCKFREE_QUEUE_SIZE); \
        QUEUE_ASSERT_VOID((queue->index_mask & LOCKFREE_QUEUE_SIZE) == 0); \
    } \
    \
    /* Destroy queue (no-op for fixed array, provided for API consistency) */ \
    void NAME##_destroy(NAME##_t* queue) { \
        QUEUE_ASSERT_VOID(queue != NULL); \
        /* Nothing to free for fixed-size array */ \
        /* Could add final stats reporting here in debug mode */ \
    } \
    \
    /* Enqueue element (returns false if queue is full) */ \
    bool NAME##_enqueue(NAME##_t* queue, const TYPE* item) { \
        /* Rule 7: Parameter validation */ \
        if (queue == NULL || item == NULL) { \
            return false; \
        } \
        \
        const size_t current_tail = atomic_load_explicit(&queue->tail, memory_order_relaxed); \
        \
        /* Rule 5: Invariant check */ \
        QUEUE_ASSERT(current_tail < LOCKFREE_QUEUE_SIZE); \
        \
        const size_t next_tail = (current_tail + 1) & queue->index_mask; \
        \
        if (next_tail == atomic_load_explicit(&queue->head, memory_order_acquire)) { \
            /* Queue is full */ \
            atomic_fetch_add(&queue->stats.failed_enqueues, 1); \
            return false; \
        } \
        \
        queue->buffer[current_tail] = *item; \
        atomic_store_explicit(&queue->tail, next_tail, memory_order_release); \
        \
        /* Update stats */ \
        atomic_fetch_add(&queue->stats.total_enqueues, 1); \
        \
        /* Update peak size if in debug mode */ \
        size_t current_size = NAME##_size(queue); \
        if (current_size > queue->stats.peak_size) { \
            queue->stats.peak_size = current_size; \
        } \
        \
        return true; \
    } \
    \
    /* Dequeue element (returns false if queue is empty) */ \
    bool NAME##_dequeue(NAME##_t* queue, TYPE* item) { \
        /* Rule 7: Parameter validation */ \
        if (queue == NULL || item == NULL) { \
            return false; \
        } \
        \
        const size_t current_head = atomic_load_explicit(&queue->head, memory_order_relaxed); \
        \
        /* Rule 5: Invariant check */ \
        QUEUE_ASSERT(current_head < LOCKFREE_QUEUE_SIZE); \
        \
        if (current_head == atomic_load_explicit(&queue->tail, memory_order_acquire)) { \
            /* Queue is empty */ \
            atomic_fetch_add(&queue->stats.failed_dequeues, 1); \
            return false; \
        } \
        \
        *item = queue->buffer[current_head]; \
        atomic_store_explicit(&queue->head, \
                            (current_head + 1) & queue->index_mask, \
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
        /* Rule 7: Parameter validation */ \
        if (queue == NULL) { \
            return true; /* Treat NULL as empty */ \
        } \
        \
        return atomic_load_explicit(&queue->head, memory_order_acquire) == \
               atomic_load_explicit(&queue->tail, memory_order_acquire); \
    } \
    \
    /* Get approximate size (may be stale due to concurrent access) */ \
    size_t NAME##_size(const NAME##_t* queue) { \
        /* Rule 7: Parameter validation */ \
        if (queue == NULL) { \
            return 0; \
        } \
        \
        const size_t current_head = atomic_load_explicit(&queue->head, memory_order_acquire); \
        const size_t current_tail = atomic_load_explicit(&queue->tail, memory_order_acquire); \
        \
        /* Rule 5: Invariants */ \
        assert(current_head < LOCKFREE_QUEUE_SIZE); \
        assert(current_tail < LOCKFREE_QUEUE_SIZE); \
        \
        return (current_tail - current_head) & queue->index_mask; \
    } \
    \
    /* Get capacity */ \
    size_t NAME##_capacity(const NAME##_t* queue) { \
        return (queue != NULL) ? queue->capacity : 0; \
    } \
    \
    /* Rule 5: Verify queue invariants (for testing/debugging) */ \
    bool NAME##_verify_invariants(const NAME##_t* queue) { \
        if (queue == NULL) return false; \
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
        size_t size = (tail - head) & queue->index_mask; \
        if (size >= LOCKFREE_QUEUE_SIZE) { \
            fprintf(stderr, "Queue size calculation error: size=%zu\n", size); \
            return false; \
        } \
        \
        /* Verify mask is correct */ \
        if (queue->index_mask != LOCKFREE_QUEUE_SIZE - 1) { \
            fprintf(stderr, "Queue mask incorrect: mask=%zu\n", queue->index_mask); \
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
        if (queue == NULL) return; \
        \
        if (total_enq) *total_enq = atomic_load(&queue->stats.total_enqueues); \
        if (total_deq) *total_deq = atomic_load(&queue->stats.total_dequeues); \
        if (failed_enq) *failed_enq = atomic_load(&queue->stats.failed_enqueues); \
        if (failed_deq) *failed_deq = atomic_load(&queue->stats.failed_dequeues); \
        if (peak) *peak = queue->stats.peak_size; \
    }

/* Legacy aliases for backward compatibility */
#define IMPLEMENT_LOCKFREE_QUEUE(TYPE, NAME) DEFINE_LOCKFREE_QUEUE(TYPE, NAME)

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_LOCKFREE_QUEUE_H */

#ifndef MATCHING_ENGINE_LOCKFREE_QUEUE_H
#define MATCHING_ENGINE_LOCKFREE_QUEUE_H

#include <stdatomic.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * CRITICAL: Increased default size from 4096 to 16384 to handle UDP bursts
 * 
 * Usage pattern:
 * 1. Define queue type with DECLARE_LOCKFREE_QUEUE macro
 * 2. Implement queue functions with IMPLEMENT_LOCKFREE_QUEUE macro
 * 3. Use generated push/pop/empty/size functions
 */

/* Cache line size (typically 64 bytes on modern CPUs) */
#define CACHE_LINE_SIZE 64

/* Default queue size (must be power of 2) */
#define LOCKFREE_QUEUE_SIZE 16384

/* Padding macro to prevent false sharing */
#define CACHE_LINE_PAD(n) char _pad##n[CACHE_LINE_SIZE]

/**
 * Macro to declare a lock-free queue type
 * 
 * Usage:
 *   DECLARE_LOCKFREE_QUEUE(input_msg_t, input_queue)
 * 
 * Generates:
 *   - struct input_queue_t { ... }
 *   - void input_queue_init(input_queue_t* q);
 *   - bool input_queue_push(input_queue_t* q, const input_msg_t* item);
 *   - bool input_queue_pop(input_queue_t* q, input_msg_t* item);
 *   - bool input_queue_empty(const input_queue_t* q);
 *   - size_t input_queue_size(const input_queue_t* q);
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
        /* Ring buffer storage */ \
        TYPE buffer[LOCKFREE_QUEUE_SIZE]; \
        \
        /* Capacity (stored for convenience) */ \
        size_t capacity; \
    } NAME##_t; \
    \
    /* Function declarations */ \
    void NAME##_init(NAME##_t* queue); \
    bool NAME##_push(NAME##_t* queue, const TYPE* item); \
    bool NAME##_pop(NAME##_t* queue, TYPE* item); \
    bool NAME##_empty(const NAME##_t* queue); \
    size_t NAME##_size(const NAME##_t* queue); \
    size_t NAME##_capacity(const NAME##_t* queue);

/**
 * Macro to implement lock-free queue functions
 * 
 * Usage (in .c file):
 *   IMPLEMENT_LOCKFREE_QUEUE(input_msg_t, input_queue)
 */
#define IMPLEMENT_LOCKFREE_QUEUE(TYPE, NAME) \
    /* Initialize queue */ \
    void NAME##_init(NAME##_t* queue) { \
        atomic_init(&queue->head, 0); \
        atomic_init(&queue->tail, 0); \
        queue->capacity = LOCKFREE_QUEUE_SIZE; \
    } \
    \
    /* Push element (returns false if queue is full) */ \
    bool NAME##_push(NAME##_t* queue, const TYPE* item) { \
        const size_t current_tail = atomic_load_explicit(&queue->tail, memory_order_relaxed); \
        const size_t next_tail = (current_tail + 1) & (LOCKFREE_QUEUE_SIZE - 1); \
        \
        if (next_tail == atomic_load_explicit(&queue->head, memory_order_acquire)) { \
            return false; /* Queue is full */ \
        } \
        \
        queue->buffer[current_tail] = *item; \
        atomic_store_explicit(&queue->tail, next_tail, memory_order_release); \
        return true; \
    } \
    \
    /* Pop element (returns false if queue is empty) */ \
    bool NAME##_pop(NAME##_t* queue, TYPE* item) { \
        const size_t current_head = atomic_load_explicit(&queue->head, memory_order_relaxed); \
        \
        if (current_head == atomic_load_explicit(&queue->tail, memory_order_acquire)) { \
            return false; /* Queue is empty */ \
        } \
        \
        *item = queue->buffer[current_head]; \
        atomic_store_explicit(&queue->head, (current_head + 1) & (LOCKFREE_QUEUE_SIZE - 1), memory_order_release); \
        return true; \
    } \
    \
    /* Check if queue is empty */ \
    bool NAME##_empty(const NAME##_t* queue) { \
        return atomic_load_explicit(&queue->head, memory_order_acquire) == \
               atomic_load_explicit(&queue->tail, memory_order_acquire); \
    } \
    \
    /* Get approximate size (may be stale due to concurrent access) */ \
    size_t NAME##_size(const NAME##_t* queue) { \
        const size_t current_head = atomic_load_explicit(&queue->head, memory_order_acquire); \
        const size_t current_tail = atomic_load_explicit(&queue->tail, memory_order_acquire); \
        return (current_tail - current_head) & (LOCKFREE_QUEUE_SIZE - 1); \
    } \
    \
    /* Get capacity */ \
    size_t NAME##_capacity(const NAME##_t* queue) { \
        return queue->capacity; \
    }
#ifdef __cplusplus

}
#endif

#endif /* MATCHING_ENGINE_LOCKFREE_QUEUE_H */

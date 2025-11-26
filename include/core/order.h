#ifndef MATCHING_ENGINE_ORDER_H
#define MATCHING_ENGINE_ORDER_H

#include "protocol/message_types.h"
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdalign.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Timestamp Implementation
 * ============================================================================
 * 
 * For HFT systems, clock_gettime() is too slow (~20-50ns syscall overhead).
 * Options in order of preference:
 *   1. RDTSC (x86) - ~5-10 cycles, requires calibration
 *   2. CLOCK_MONOTONIC_COARSE - ~5ns but millisecond precision
 *   3. Cached timestamp from timer thread - amortized cost
 *   4. clock_gettime(CLOCK_MONOTONIC) - baseline fallback
 *
 * We use RDTSC on x86-64 Linux, fallback to clock_gettime elsewhere.
 * For strict ordering, RDTSC is sufficient - we only need monotonicity.
 * ============================================================================ */

#if defined(__x86_64__) && defined(__linux__)
    #define USE_RDTSC 1
#else
    #define USE_RDTSC 0
#endif

#if USE_RDTSC
/**
 * Read CPU timestamp counter - extremely fast (~5-10 cycles)
 * Note: On modern CPUs with invariant TSC, this is reliable for ordering.
 * For wall-clock time, would need calibration against clock_gettime.
 */
static inline uint64_t order_get_current_timestamp(void) {
    uint32_t lo, hi;
    __asm__ volatile (
        "rdtsc"
        : "=a" (lo), "=d" (hi)
    );
    return ((uint64_t)hi << 32) | lo;
}
#else
/**
 * Fallback: clock_gettime for non-x86 or non-Linux platforms
 */
static inline uint64_t order_get_current_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

/* ============================================================================
 * Order Structure - Cache-Line Aligned
 * ============================================================================
 *
 * Design decisions:
 * - Aligned to 64-byte cache line to prevent false sharing
 * - No symbol field - order book is single-symbol, symbol stored in book
 * - Hot fields (accessed during matching) packed together
 * - Uses uint64_t timestamp for time priority
 * - Tracks remaining_qty separately for partial fills
 *
 * Memory layout (64 bytes total, fits in one cache line):
 *   Bytes 0-3:   user_id
 *   Bytes 4-7:   user_order_id
 *   Bytes 8-11:  price
 *   Bytes 12-15: quantity
 *   Bytes 16-19: remaining_qty
 *   Bytes 20:    side (uint8_t)
 *   Bytes 21:    type (uint8_t)
 *   Bytes 22-23: padding
 *   Bytes 24-27: client_id
 *   Bytes 28-31: padding
 *   Bytes 32-39: timestamp
 *   Bytes 40-47: next pointer
 *   Bytes 48-55: prev pointer
 *   Bytes 56-63: padding
 * ============================================================================ */

/* Alignment macro - works on GCC/Clang */
#if defined(__GNUC__) || defined(__clang__)
    #define CACHE_ALIGNED __attribute__((aligned(64)))
#else
    #define CACHE_ALIGNED _Alignas(64)
#endif

typedef struct order {
    /* Hot path fields - accessed during matching */
    uint32_t user_id;           /* 0-3 */
    uint32_t user_order_id;     /* 4-7 */
    uint32_t price;             /* 8-11: 0 = market order, >0 = limit order */
    uint32_t quantity;          /* 12-15: Original quantity */
    uint32_t remaining_qty;     /* 16-19: Remaining unfilled quantity */
    side_t side;                /* 20: Buy or Sell (uint8_t) */
    order_type_t type;          /* 21: Market or Limit (uint8_t) */
    uint8_t _pad1[2];           /* 22-23: Explicit padding for alignment */
    uint32_t client_id;         /* 24-27: 0 for UDP, client_id for TCP */
    uint32_t _pad2;             /* 28-31: Padding for uint64_t alignment */
    
    /* Time priority */
    uint64_t timestamp;         /* 32-39 */
    
    /* Linked list pointers */
    struct order *next;         /* 40-47 */
    struct order *prev;         /* 48-55 */
    
    /* Padding to ensure 64-byte total size */
    uint8_t _padding[8];        /* 56-63 */
} CACHE_ALIGNED order_t;

/* Compile-time assertion to verify size and alignment */
_Static_assert(sizeof(order_t) == 64, "order_t must be exactly 64 bytes (one cache line)");
#if defined(__GNUC__) || defined(__clang__)
_Static_assert(alignof(order_t) == 64, "order_t must be 64-byte aligned");
#endif

/**
 * Initialize order from new order message
 * Note: Symbol is NOT stored in order - it's in the order book
 */
static inline void order_init(order_t* order, const new_order_msg_t* msg, uint64_t timestamp) {
    /* Rule 5: Assertions for parameter validation */
    assert(order != NULL && "NULL order in order_init");
    assert(msg != NULL && "NULL message in order_init");
    assert(msg->quantity > 0 && "Zero quantity order");
    
    order->user_id = msg->user_id;
    order->user_order_id = msg->user_order_id;
    order->price = msg->price;
    order->quantity = msg->quantity;
    order->remaining_qty = msg->quantity;
    order->side = msg->side;
    order->type = (msg->price == 0) ? ORDER_TYPE_MARKET : ORDER_TYPE_LIMIT;
    order->client_id = 0;  /* Set by caller for TCP mode */
    order->timestamp = timestamp;
    order->next = NULL;
    order->prev = NULL;
}

/**
 * Check if order is fully filled
 */
static inline bool order_is_filled(const order_t* order) {
    assert(order != NULL && "NULL order in order_is_filled");
    return order->remaining_qty == 0;
}

/**
 * Fill order by quantity, returns amount actually filled
 */
static inline uint32_t order_fill(order_t* order, uint32_t qty) {
    assert(order != NULL && "NULL order in order_fill");
    assert(qty > 0 && "Zero fill quantity");
    assert(order->remaining_qty >= qty && "Overfill attempt");
    
    uint32_t filled = (qty < order->remaining_qty) ? qty : order->remaining_qty;
    order->remaining_qty -= filled;
    return filled;
}

/**
 * Get order priority for comparison (price-time priority)
 * Lower value = higher priority
 * For bids: higher price is better, so negate price
 * For asks: lower price is better
 */
static inline int64_t order_get_priority(const order_t* order, bool is_bid) {
    assert(order != NULL && "NULL order in order_get_priority");
    
    if (is_bid) {
        /* Bids: higher price = higher priority (negate for min-ordering) */
        return -((int64_t)order->price);
    } else {
        /* Asks: lower price = higher priority */
        return (int64_t)order->price;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_ORDER_H */

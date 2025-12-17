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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Timestamp Implementation
 * ============================================================================
 * 
 * For HFT systems, clock_gettime() is too slow (~20-50ns syscall overhead).
 * Options in order of preference:
 *   1. RDTSCP (x86) - ~5-10 cycles, self-serializing
 *   2. CLOCK_MONOTONIC_COARSE - ~5ns but millisecond precision
 *   3. Cached timestamp from timer thread - amortized cost
 *   4. clock_gettime(CLOCK_MONOTONIC) - baseline fallback
 *
 * Platform support:
 *   - Linux x86-64:        RDTSCP (fast path)
 *   - macOS x86-64:        RDTSCP (fast path)
 *   - macOS ARM64 (M1+):   clock_gettime (fallback)
 *   - Linux ARM64:         clock_gettime (fallback)
 *
 * For strict ordering, RDTSCP is sufficient - we only need monotonicity.
 * RDTSCP is self-serializing, unlike RDTSC which can be reordered.
 * ============================================================================ */

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
    #define USE_RDTSC 1
#else
    #define USE_RDTSC 0
#endif

#if USE_RDTSC
/**
 * Read CPU timestamp counter with serialization (x86-64 only)
 * 
 * RDTSCP is self-serializing and returns processor ID in ECX (ignored).
 * Available on Intel Core 2+ (2007) and AMD K10+ (2007).
 * 
 * Performance: ~5-10 CPU cycles
 * 
 * Note: On modern CPUs with invariant TSC, this is reliable for ordering.
 * For wall-clock time conversion, would need calibration against clock_gettime.
 */
static inline uint64_t order_get_current_timestamp(void) {
    uint32_t lo, hi, aux;
    __asm__ volatile (
        "rdtscp"
        : "=a" (lo), "=d" (hi), "=c" (aux)
    );
    (void)aux;  /* Processor ID - unused */
    return ((uint64_t)hi << 32) | lo;
}

#else
/**
 * Fallback: clock_gettime for non-x86 platforms
 * 
 * Works on: Linux (all archs), macOS 10.12+ (including Apple Silicon)
 * 
 * Performance: ~20-50ns (syscall overhead)
 */
static inline uint64_t order_get_current_timestamp(void) {
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0 && "clock_gettime failed");
    (void)rc;  /* Suppress unused warning when NDEBUG defined */
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
 *   Bytes 22-23: padding (align to 4-byte boundary)
 *   Bytes 24-27: client_id
 *   Bytes 28-31: padding (align timestamp to 8-byte boundary)
 *   Bytes 32-39: timestamp
 *   Bytes 40-47: next pointer
 *   Bytes 48-55: prev pointer
 *   Bytes 56-63: padding (pad to cache line)
 * ============================================================================ */

/* Cache line alignment macro - 64 bytes on modern x86/ARM */
#if defined(__GNUC__) || defined(__clang__)
    #define CACHE_ALIGNED __attribute__((aligned(64)))
#elif defined(_MSC_VER)
    #define CACHE_ALIGNED __declspec(align(64))
#else
    #error "Unsupported compiler: no cache alignment primitive available"
#endif

typedef struct order {
    /* === Cache Line 0 (64 bytes) === */
    
    /* Hot fields - accessed during matching (bytes 0-19) */
    uint32_t user_id;           /* 0-3:   Order owner */
    uint32_t user_order_id;     /* 4-7:   Owner's order sequence number */
    uint32_t price;             /* 8-11:  0 = market order, >0 = limit price */
    uint32_t quantity;          /* 12-15: Original quantity */
    uint32_t remaining_qty;     /* 16-19: Unfilled quantity */
    
    /* Order metadata (bytes 20-31) */
    side_t side;                /* 20:    SIDE_BUY or SIDE_SELL */
    order_type_t type;          /* 21:    ORDER_TYPE_MARKET or ORDER_TYPE_LIMIT */
    uint8_t _pad1[2];           /* 22-23: Align next field to 4-byte boundary */
    uint32_t client_id;         /* 24-27: TCP client ID (0 for UDP) */
    uint32_t _pad2;             /* 28-31: Align timestamp to 8-byte boundary */
    
    /* Time priority (bytes 32-39) */
    uint64_t timestamp;         /* 32-39: RDTSC cycles or nanoseconds */
    
    /* Linked list pointers (bytes 40-55) */
    struct order *next;         /* 40-47: Next order at same price level */
    struct order *prev;         /* 48-55: Previous order at same price level */
    
    /* Explicit padding to cache line (bytes 56-63) */
    uint8_t _pad3[8];           /* 56-63: Pad to exactly 64 bytes */
    
} CACHE_ALIGNED order_t;

/* Compile-time assertions to verify size and alignment */
_Static_assert(sizeof(order_t) == 64, "order_t must be exactly 64 bytes (one cache line)");
_Static_assert(alignof(order_t) == 64, "order_t must be 64-byte aligned");

/* Verify field offsets match documented layout */
_Static_assert(offsetof(order_t, user_id) == 0, "user_id offset mismatch");
_Static_assert(offsetof(order_t, timestamp) == 32, "timestamp offset mismatch");
_Static_assert(offsetof(order_t, next) == 40, "next pointer offset mismatch");
_Static_assert(offsetof(order_t, prev) == 48, "prev pointer offset mismatch");

/**
 * Initialize order from new order message
 * 
 * Note: Symbol is NOT stored in order - it's stored in the order book.
 * Note: client_id is set to 0 here; caller must set it for TCP mode.
 * 
 * @param order     Order to initialize (must not be NULL)
 * @param msg       New order message (must not be NULL, quantity > 0)
 * @param timestamp Timestamp from order_get_current_timestamp()
 */
static inline void order_init(order_t* order, const new_order_msg_t* msg, uint64_t timestamp) {
    /* Preconditions (Rule 5) */
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
    
    /* Postconditions (Rule 5) - verify invariants hold */
    assert(order->remaining_qty == order->quantity && 
           "Init failed: remaining_qty != quantity");
    assert((order->type == ORDER_TYPE_MARKET) == (order->price == 0) && 
           "Init failed: type/price mismatch");
}

/**
 * Check if order is fully filled
 * 
 * @param order Order to check (must not be NULL)
 * @return true if remaining_qty == 0
 */
static inline bool order_is_filled(const order_t* order) {
    /* Preconditions (Rule 5) */
    assert(order != NULL && "NULL order in order_is_filled");
    assert(order->remaining_qty <= order->quantity && 
           "Invariant violation: remaining_qty exceeds original quantity");
    
    return order->remaining_qty == 0;
}

/**
 * Fill order by quantity, returns amount actually filled
 * 
 * @param order Order to fill (must not be NULL)
 * @param qty   Quantity to fill (must be > 0 and <= remaining_qty)
 * @return Amount actually filled (always == qty given preconditions)
 */
static inline uint32_t order_fill(order_t* order, uint32_t qty) {
    /* Preconditions (Rule 5) */
    assert(order != NULL && "NULL order in order_fill");
    assert(qty > 0 && "Zero fill quantity");
    assert(order->remaining_qty >= qty && "Overfill attempt");
    
    /* Save for postcondition check */
    uint32_t prev_remaining = order->remaining_qty;
    
    uint32_t filled = (qty < order->remaining_qty) ? qty : order->remaining_qty;
    order->remaining_qty -= filled;
    
    /* Postcondition (Rule 5) */
    assert(order->remaining_qty == prev_remaining - filled && 
           "Fill arithmetic error");
    
    return filled;
}

/**
 * Get order priority for comparison (price-time priority)
 * 
 * Returns a value where lower = higher priority.
 * For bids: higher price is better, so we negate
 * For asks: lower price is better
 * 
 * Time priority is handled separately (compare timestamps when prices equal).
 * 
 * @param order  Order to get priority for (must not be NULL)
 * @param is_bid true for buy orders, false for sell orders
 * @return Priority value (lower = higher priority)
 */
static inline int64_t order_get_priority(const order_t* order, bool is_bid) {
    /* Preconditions (Rule 5) */
    assert(order != NULL && "NULL order in order_get_priority");
    assert((order->type == ORDER_TYPE_MARKET || order->price > 0) && 
           "Limit order with zero price");
    
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

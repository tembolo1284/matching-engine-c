#ifndef MATCHING_ENGINE_ORDER_BOOK_H
#define MATCHING_ENGINE_ORDER_BOOK_H

#include "core/order.h"
#include "protocol/message_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdalign.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OrderBook - Single symbol order book with price-time priority
 *
 * Design principles (Power of Ten + Roman Bansal's rules):
 * - No dynamic allocation after init (Rule 3)
 * - All loops have fixed upper bounds (Rule 2)
 * - Open-addressing hash table for cache-friendly lookups
 * - Cache-line aligned orders to prevent false sharing
 * - Pre-allocated memory pools
 */

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/* Maximum price levels we can handle */
#define MAX_PRICE_LEVELS 10000

/* Maximum orders per price level (for capacity planning) */
#define TYPICAL_ORDERS_PER_LEVEL 20

/* Maximum output messages from a single operation */
#define MAX_OUTPUT_MESSAGES 8192

/* Number of orders to process per flush iteration (for batched flush) */
#define FLUSH_BATCH_SIZE 4096

/*
 * Hash table size - MUST be power of 2 for fast masking
 * Load factor target: ~50% for good probe performance
 */
#define ORDER_MAP_SIZE 524288
#define ORDER_MAP_MASK (ORDER_MAP_SIZE - 1)

/* Maximum probe length for open-addressing (Rule 2 compliance) */
#define MAX_PROBE_LENGTH 128

/* Maximum iterations for matching loops (Rule 2 compliance) */
#define MAX_MATCH_ITERATIONS (MAX_PRICE_LEVELS * TYPICAL_ORDERS_PER_LEVEL)
#define MAX_ORDERS_AT_PRICE_LEVEL (TYPICAL_ORDERS_PER_LEVEL * 10)

/* Memory pool sizes */
#define MAX_ORDERS_IN_POOL 262144

/* Sentinel values for open-addressing hash table */
#define HASH_SLOT_EMPTY     0ULL
#define HASH_SLOT_TOMBSTONE UINT64_MAX

/* ============================================================================
 * Core Data Structures
 * ============================================================================ */

/**
 * Price level - holds orders at a specific price
 * Aligned to avoid false sharing between adjacent levels
 */
#if defined(__GNUC__) || defined(__clang__)
typedef struct __attribute__((aligned(64))) {
#else
typedef struct {
#endif
    uint32_t price;
    uint32_t total_quantity;
    order_t* orders_head;
    order_t* orders_tail;
    bool active;
    uint8_t _padding[64 - sizeof(uint32_t)*2 - sizeof(order_t*)*2 - sizeof(bool)];
} price_level_t;

_Static_assert(sizeof(price_level_t) == 64, "price_level_t should be cache-line aligned");

/**
 * Order location for cancellation lookup
 */
typedef struct {
    side_t side;
    uint32_t price;
    order_t* order_ptr;
} order_location_t;

/**
 * Open-addressing hash table slot
 * Key = 0 means empty, Key = UINT64_MAX means tombstone (deleted)
 */
typedef struct {
    uint64_t key;               /* Combined user_id + user_order_id */
    order_location_t location;
} order_map_slot_t;

/**
 * Open-addressing hash table for order lookup
 * - No pointer chasing = cache-friendly
 * - Linear probing for locality
 * - Power-of-2 size for fast modulo via masking
 */
typedef struct {
    order_map_slot_t slots[ORDER_MAP_SIZE];
    uint32_t count;             /* Number of active entries */
    uint32_t tombstone_count;   /* Number of tombstones (for rehash decision) */
} order_map_t;

/* ============================================================================
 * Memory Pool Structure (Simplified - no hash entry pool needed)
 * ============================================================================ */

/**
 * Pre-allocated memory pool for orders
 * All orders come from this pool - zero malloc in hot path
 */
typedef struct {
    order_t orders[MAX_ORDERS_IN_POOL];
    uint32_t free_list[MAX_ORDERS_IN_POOL];
    int free_count;
    uint32_t total_allocations;
    uint32_t peak_usage;
    uint32_t allocation_failures;
} order_pool_t;

/**
 * Memory pools container (simplified - only order pool now)
 */
typedef struct {
    order_pool_t order_pool;
} memory_pools_t;

/* ============================================================================
 * Flush State for Iterative Flushing
 * ============================================================================ */

/**
 * Flush state - tracks progress through iterative flush
 * Allows flushing large books in batches without buffer overflow
 */
typedef struct {
    bool in_progress;           /* True if flush is ongoing */
    int current_bid_level;      /* Current bid level being processed */
    int current_ask_level;      /* Current ask level being processed */
    order_t* current_order;     /* Current order within level (NULL = start of level) */
    bool processing_bids;       /* True = processing bids, False = processing asks */
    bool bids_done;             /* True if all bids have been processed */
    bool asks_done;             /* True if all asks have been processed */
} flush_state_t;

/* ============================================================================
 * Order Book Structure
 * ============================================================================ */

/**
 * Order book structure
 */
typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];

    /* Price levels - fixed arrays */
    price_level_t bids[MAX_PRICE_LEVELS];
    price_level_t asks[MAX_PRICE_LEVELS];
    int num_bid_levels;
    int num_ask_levels;

    /* Order lookup - open-addressing hash table */
    order_map_t order_map;

    /* Track previous best bid/ask for TOB change detection */
    uint32_t prev_best_bid_price;
    uint32_t prev_best_bid_qty;
    uint32_t prev_best_ask_price;
    uint32_t prev_best_ask_qty;

    /* Track if sides ever had orders */
    bool bid_side_ever_active;
    bool ask_side_ever_active;

    /* Flush state for iterative flushing */
    flush_state_t flush_state;

    /* Memory pools */
    memory_pools_t* pools;

} order_book_t;

/**
 * Output buffer for messages
 */
typedef struct {
    output_msg_t messages[MAX_OUTPUT_MESSAGES];
    int count;
} output_buffer_t;

/* ============================================================================
 * Memory Pool API
 * ============================================================================ */

/**
 * Initialize all memory pools (called once at startup)
 */
void memory_pools_init(memory_pools_t* pools);

/**
 * Get memory pool statistics
 */
typedef struct {
    uint32_t order_allocations;
    uint32_t order_peak_usage;
    uint32_t order_failures;
    uint32_t hash_count;        /* Current entries in hash table */
    uint32_t hash_tombstones;   /* Tombstone count */
    size_t total_memory_bytes;
} memory_pool_stats_t;

void memory_pools_get_stats(const memory_pools_t* pools,
                            const order_book_t* book,
                            memory_pool_stats_t* stats);

/* ============================================================================
 * Order Book Public API
 * ============================================================================ */

void order_book_init(order_book_t* book, const char* symbol, memory_pools_t* pools);
void order_book_destroy(order_book_t* book);

void order_book_add_order(order_book_t* book,
                          const new_order_msg_t* msg,
                          uint32_t client_id,
                          output_buffer_t* output);

void order_book_cancel_order(order_book_t* book,
                              uint32_t user_id,
                              uint32_t user_order_id,
                              output_buffer_t* output);

/**
 * Flush order book - iterative version
 * 
 * Processes up to FLUSH_BATCH_SIZE orders per call.
 * Returns true when flush is complete, false if more iterations needed.
 * 
 * Usage:
 *   while (!order_book_flush(book, output)) {
 *       // drain output buffer
 *   }
 */
bool order_book_flush(order_book_t* book, output_buffer_t* output);

/**
 * Check if flush is in progress
 */
static inline bool order_book_flush_in_progress(const order_book_t* book) {
    return book->flush_state.in_progress;
}

/**
 * Reset flush state (cancel an in-progress flush)
 */
void order_book_flush_reset(order_book_t* book);

size_t order_book_cancel_client_orders(order_book_t* book,
                                       uint32_t client_id,
                                       output_buffer_t* output);

uint32_t order_book_get_best_bid_price(const order_book_t* book);
uint32_t order_book_get_best_ask_price(const order_book_t* book);
uint32_t order_book_get_best_bid_quantity(const order_book_t* book);
uint32_t order_book_get_best_ask_quantity(const order_book_t* book);

/* ============================================================================
 * Helper Functions (Inline)
 * ============================================================================ */

static inline void output_buffer_init(output_buffer_t* buf) {
    buf->count = 0;
}

static inline bool output_buffer_has_space(const output_buffer_t* buf, int needed) {
    return (buf->count + needed) <= MAX_OUTPUT_MESSAGES;
}

static inline void output_buffer_add(output_buffer_t* buf, const output_msg_t* msg) {
    assert(buf != NULL && "NULL buffer in output_buffer_add");
    assert(msg != NULL && "NULL message in output_buffer_add");

    if (buf->count < MAX_OUTPUT_MESSAGES) {
        buf->messages[buf->count++] = *msg;
    } else {
        /* Rule 5: Log overflow but don't crash */
        #ifdef DEBUG
        fprintf(stderr, "WARNING: Output buffer overflow\n");
        #endif
    }
}

/**
 * Create order key from user_id and user_order_id
 * This is the key used in the hash table
 */
static inline uint64_t make_order_key(uint32_t user_id, uint32_t user_order_id) {
    return ((uint64_t)user_id << 32) | user_order_id;
}

/**
 * Fast hash function using multiply-shift
 * Based on Knuth's multiplicative hash
 * Much faster than modulo, and mixes bits well
 */
static inline uint32_t hash_order_key(uint64_t key) {
    /* Golden ratio constant for 64-bit */
    const uint64_t GOLDEN_RATIO = 0x9E3779B97F4A7C15ULL;

    /* Multiply and take high bits - mixes all input bits */
    key ^= key >> 33;
    key *= GOLDEN_RATIO;
    key ^= key >> 29;

    /* Mask to table size (power of 2) */
    return (uint32_t)(key & ORDER_MAP_MASK);
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_ORDER_BOOK_H */

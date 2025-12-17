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
 * Design principles (Power of Ten + cache optimization):
 * - No dynamic allocation after init (Rule 3)
 * - All loops have fixed upper bounds (Rule 2)
 * - Minimum 2 assertions per function (Rule 5)
 * - Open-addressing hash table for cache-friendly lookups
 * - Cache-line aligned orders to prevent false sharing
 * - Pre-allocated memory pools
 */

/* ============================================================================
 * Cache Alignment Macro
 * ============================================================================ */

#ifndef CACHE_ALIGNED
    #if defined(__GNUC__) || defined(__clang__)
        #define CACHE_ALIGNED __attribute__((aligned(64)))
    #elif defined(_MSC_VER)
        #define CACHE_ALIGNED __declspec(align(64))
    #else
        #error "Unsupported compiler: no cache alignment primitive available"
    #endif
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/* Maximum price levels we can handle */
#define MAX_PRICE_LEVELS 512

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
#define ORDER_MAP_SIZE 16384
#define ORDER_MAP_MASK (ORDER_MAP_SIZE - 1)

/* Compile-time verification that sizes are powers of 2 */
_Static_assert((ORDER_MAP_SIZE & (ORDER_MAP_SIZE - 1)) == 0,
               "ORDER_MAP_SIZE must be power of 2 for mask to work");

/* Maximum probe length for open-addressing (Rule 2 compliance) */
#define MAX_PROBE_LENGTH 128

/* Maximum iterations for matching loops (Rule 2 compliance) */
#define MAX_MATCH_ITERATIONS (MAX_PRICE_LEVELS * TYPICAL_ORDERS_PER_LEVEL)
#define MAX_ORDERS_AT_PRICE_LEVEL (TYPICAL_ORDERS_PER_LEVEL * 10)

/* Memory pool sizes */
#define MAX_ORDERS_IN_POOL 8192

/* 
 * Sentinel values for open-addressing hash table
 * 
 * HASH_SLOT_EMPTY (0): Slot has never been used. Terminates probe sequence.
 * HASH_SLOT_TOMBSTONE (UINT64_MAX): Slot was deleted. Continue probing.
 * 
 * IMPORTANT: make_order_key() must never return these values.
 * With user_id in high 32 bits and user_order_id in low 32 bits:
 *   - 0 only occurs if both are 0 (invalid user)
 *   - UINT64_MAX only occurs if both are UINT32_MAX (practically impossible)
 */
#define HASH_SLOT_EMPTY     0ULL
#define HASH_SLOT_TOMBSTONE UINT64_MAX

/* ============================================================================
 * Core Data Structures
 * ============================================================================ */

/**
 * Price level - holds orders at a specific price
 * Aligned to 64 bytes to avoid false sharing between adjacent levels
 * 
 * Padding calculation (64-bit system):
 *   uint32_t price:          4 bytes
 *   uint32_t total_quantity: 4 bytes  
 *   order_t* orders_head:    8 bytes
 *   order_t* orders_tail:    8 bytes
 *   bool active:             1 byte
 *   ---------------------------------
 *   Subtotal:               25 bytes
 *   Padding needed:         39 bytes (to reach 64)
 */
typedef struct {
    uint32_t price;             /* Price for this level */
    uint32_t total_quantity;    /* Sum of remaining_qty for all orders */
    order_t* orders_head;       /* First order (oldest, highest time priority) */
    order_t* orders_tail;       /* Last order (newest) */
    bool active;                /* True if level has orders */
    uint8_t _pad[39];           /* Explicit padding to 64 bytes */
} price_level_t CACHE_ALIGNED;

_Static_assert(sizeof(price_level_t) == 64, "price_level_t must be cache-line sized");

/**
 * Order location for cancellation lookup
 */
typedef struct {
    side_t side;                /* SIDE_BUY or SIDE_SELL */
    uint32_t price;             /* Price level where order resides */
    order_t* order_ptr;         /* Direct pointer to order */
} order_location_t;

/**
 * Open-addressing hash table slot
 * Key = 0 means empty, Key = UINT64_MAX means tombstone (deleted)
 */
typedef struct {
    uint64_t key;               /* Combined user_id + user_order_id */
    order_location_t location;  /* Where to find the order */
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
 * Memory Pool Structure
 * ============================================================================ */

/**
 * Pre-allocated memory pool for orders
 * All orders come from this pool - zero malloc in hot path
 */
typedef struct {
    order_t orders[MAX_ORDERS_IN_POOL];
    uint32_t free_list[MAX_ORDERS_IN_POOL];
    uint32_t free_count;        /* Number of available slots */
    uint32_t total_allocations; /* Lifetime allocation count */
    uint32_t peak_usage;        /* High water mark */
    uint32_t allocation_failures; /* Count of exhaustion events */
} order_pool_t;

/**
 * Memory pools container
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
 * 
 * Fields ordered by size (largest first) for optimal packing:
 *   order_t* (8) + uint32_t (4) + uint32_t (4) + 4*bool (4) + padding (4) = 24 bytes
 */
typedef struct {
    /* 8-byte aligned */
    order_t* current_order;     /* Current order within level (NULL = start of level) */
    
    /* 4-byte aligned */
    uint32_t current_bid_level; /* Current bid level being processed */
    uint32_t current_ask_level; /* Current ask level being processed */
    
    /* 1-byte fields (packed together) */
    bool in_progress;           /* True if flush is ongoing */
    bool processing_bids;       /* True = processing bids, False = processing asks */
    bool bids_done;             /* True if all bids have been processed */
    bool asks_done;             /* True if all asks have been processed */
} flush_state_t;

/* ============================================================================
 * Order Book Structure
 * ============================================================================ */

/**
 * Order book structure for a single symbol
 */
typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];

    /* Price levels - fixed arrays, sorted by price */
    price_level_t bids[MAX_PRICE_LEVELS];  /* Descending price order */
    price_level_t asks[MAX_PRICE_LEVELS];  /* Ascending price order */
    uint32_t num_bid_levels;
    uint32_t num_ask_levels;

    /* Order lookup - open-addressing hash table */
    order_map_t order_map;

    /* Track previous best bid/ask for TOB change detection */
    uint32_t prev_best_bid_price;
    uint32_t prev_best_bid_qty;
    uint32_t prev_best_ask_price;
    uint32_t prev_best_ask_qty;

    /* Track if sides ever had orders (for TOB eliminated messages) */
    bool bid_side_ever_active;
    bool ask_side_ever_active;

    /* Flush state for iterative flushing */
    flush_state_t flush_state;

    /* Memory pools (shared, not owned) */
    memory_pools_t* pools;

} order_book_t;

/**
 * Output buffer for messages
 */
typedef struct {
    output_msg_t messages[MAX_OUTPUT_MESSAGES];
    uint32_t count;
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
    assert(book != NULL && "NULL book in order_book_flush_in_progress");
    assert(book->pools != NULL && "Book has NULL pools pointer");
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

/**
 * Initialize output buffer
 */
static inline void output_buffer_init(output_buffer_t* buf) {
    assert(buf != NULL && "NULL buffer in output_buffer_init");
    buf->count = 0;
    assert(buf->count == 0 && "output_buffer_init failed");
}

/**
 * Check if output buffer has space for more messages
 */
static inline bool output_buffer_has_space(const output_buffer_t* buf, uint32_t needed) {
    assert(buf != NULL && "NULL buffer in output_buffer_has_space");
    assert(buf->count <= MAX_OUTPUT_MESSAGES && "Buffer count exceeds maximum");
    return (buf->count + needed) <= MAX_OUTPUT_MESSAGES;
}

/**
 * Add message to output buffer
 * Silently drops message if buffer is full (logged in DEBUG)
 */
static inline void output_buffer_add(output_buffer_t* buf, const output_msg_t* msg) {
    assert(buf != NULL && "NULL buffer in output_buffer_add");
    assert(msg != NULL && "NULL message in output_buffer_add");
    assert(buf->count <= MAX_OUTPUT_MESSAGES && "Invalid buffer count");

    if (buf->count < MAX_OUTPUT_MESSAGES) {
        buf->messages[buf->count++] = *msg;
    }
    /* Silent drop on overflow - production systems should monitor this */
}

/**
 * Create order key from user_id and user_order_id
 * This is the key used in the hash table
 * 
 * Key format: [user_id (32 bits)][user_order_id (32 bits)]
 */
static inline uint64_t make_order_key(uint32_t user_id, uint32_t user_order_id) {
    /* Preconditions: key must not collide with sentinel values */
    assert((user_id != 0 || user_order_id != 0) && 
           "Zero order key is reserved for HASH_SLOT_EMPTY");
    
    uint64_t key = ((uint64_t)user_id << 32) | user_order_id;
    
    assert(key != HASH_SLOT_TOMBSTONE && 
           "Order key collision with HASH_SLOT_TOMBSTONE");
    
    return key;
}

/**
 * Fast hash function using multiply-shift
 * Based on splitmix64 / Knuth's multiplicative hash
 * 
 * Properties:
 * - Good avalanche (small input changes affect all output bits)
 * - Fast (no division, just multiply/shift/xor)
 * - Deterministic
 */
static inline uint32_t hash_order_key(uint64_t key) {
    assert(key != HASH_SLOT_EMPTY && "Cannot hash empty key");
    assert(key != HASH_SLOT_TOMBSTONE && "Cannot hash tombstone key");
    
    /* Golden ratio constant - good bit mixing properties */
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

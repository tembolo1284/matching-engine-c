#ifndef MATCHING_ENGINE_ORDER_BOOK_H
#define MATCHING_ENGINE_ORDER_BOOK_H

#include "core/order.h"
#include "protocol/message_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OrderBook - Single symbol order book with price-time priority
 */

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/* Maximum price levels we can handle */
#define MAX_PRICE_LEVELS 10000

/* Maximum orders per price level (for capacity planning) */
#define TYPICAL_ORDERS_PER_LEVEL 20

/* Maximum output messages from a single operation */
#define MAX_OUTPUT_MESSAGES 1024

/* Hash table size for order lookup */
#define ORDER_MAP_SIZE 4096

/* Maximum iterations for matching loops (Rule 2 compliance) */
#define MAX_MATCH_ITERATIONS (MAX_PRICE_LEVELS * TYPICAL_ORDERS_PER_LEVEL)
#define MAX_ORDERS_AT_PRICE_LEVEL (TYPICAL_ORDERS_PER_LEVEL * 10)
#define MAX_HASH_CHAIN_LENGTH 100

/* Memory pool sizes */
#define MAX_ORDERS_IN_POOL 10000
#define MAX_HASH_ENTRIES_IN_POOL 10000

/* ============================================================================
 * Core Data Structures (MUST BE DEFINED FIRST)
 * ============================================================================ */

/**
 * Price level - holds orders at a specific price
 */
typedef struct {
    uint32_t price;
    uint32_t total_quantity;
    order_t* orders_head;
    order_t* orders_tail;
    bool active;
} price_level_t;

/**
 * Order location for cancellation lookup
 */
typedef struct {
    side_t side;
    uint32_t price;
    order_t* order_ptr;
} order_location_t;

/**
 * Hash table entry for order lookup
 */
typedef struct order_map_entry {
    uint64_t key;
    order_location_t location;
    struct order_map_entry* next;
} order_map_entry_t;

/**
 * Simple hash table for order lookup
 */
typedef struct {
    order_map_entry_t* buckets[ORDER_MAP_SIZE];
} order_map_t;

/* ============================================================================
 * Memory Pool Structures (NOW order_map_entry_t IS DEFINED)
 * ============================================================================ */

/**
 * Pre-allocated memory pool for orders
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
 * Pre-allocated memory pool for hash table entries
 */
typedef struct {
    order_map_entry_t entries[MAX_HASH_ENTRIES_IN_POOL];  // ← Now this works!
    uint32_t free_list[MAX_HASH_ENTRIES_IN_POOL];
    int free_count;
    uint32_t total_allocations;
    uint32_t peak_usage;
    uint32_t allocation_failures;
} hash_entry_pool_t;

/**
 * Combined memory pools for an order book
 */
typedef struct {
    order_pool_t order_pool;
    hash_entry_pool_t hash_entry_pool;
} memory_pools_t;

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

    /* Order lookup for cancellations */
    order_map_t order_map;

    /* Track previous best bid/ask for TOB change detection */
    uint32_t prev_best_bid_price;
    uint32_t prev_best_bid_qty;
    uint32_t prev_best_ask_price;
    uint32_t prev_best_ask_qty;

    /* Track if sides ever had orders */
    bool bid_side_ever_active;
    bool ask_side_ever_active;

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
    uint32_t hash_allocations;
    uint32_t hash_peak_usage;
    uint32_t hash_failures;
    size_t total_memory_bytes;
} memory_pool_stats_t;

void memory_pools_get_stats(const memory_pools_t* pools, memory_pool_stats_t* stats);

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

void order_book_flush(order_book_t* book, output_buffer_t* output);

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

static inline void output_buffer_add(output_buffer_t* buf, const output_msg_t* msg) {
    if (buf->count < MAX_OUTPUT_MESSAGES) {
        buf->messages[buf->count++] = *msg;
    }
}

static inline uint64_t make_order_key(uint32_t user_id, uint32_t user_order_id) {
    return ((uint64_t)user_id << 32) | user_order_id;
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_ORDER_BOOK_H */

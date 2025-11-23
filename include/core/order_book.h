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
 *
 * NASA Power of 10 Compliant:
 * - Rule 2: All loops have fixed upper bounds
 * - Rule 3: No malloc/free after initialization (memory pools)
 * - Rule 4: Functions ≤ 60 lines
 * - Rule 5: Assertions for invariant checking
 * - Rule 7: All parameters validated, return values checked
 *
 * Updated for TCP multi-client support:
 * - Tracks client_id with each order for ownership
 * - Includes client_id in trade messages for routing
 * - Supports cancelling all orders for a disconnected client
 *
 * Design decisions:
 * - Fixed array of price levels (10000 slots)
 * - Binary search to find price levels O(log N)
 * - Doubly-linked list for orders at each price (FIFO time priority)
 * - Bids sorted descending (best bid = highest price = index 0)
 * - Asks sorted ascending (best ask = lowest price = index 0)
 * - Hash table for fast order cancellation lookup
 * - Pre-allocated memory pools for zero runtime allocation
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

/* ============================================================================
 * Memory Pool Structures (Rule 3 Compliance - No malloc after init)
 * ============================================================================ */

/**
 * Pre-allocated memory pool for orders
 * All orders allocated from this pool at runtime (no malloc)
 */
#define MAX_ORDERS_IN_POOL 10000

typedef struct {
    order_t orders[MAX_ORDERS_IN_POOL];           // Pre-allocated order storage
    uint32_t free_list[MAX_ORDERS_IN_POOL];       // Stack of free indices
    int free_count;                                // Number of available slots
    
    // Statistics (for monitoring)
    uint32_t total_allocations;
    uint32_t peak_usage;
    uint32_t allocation_failures;
} order_pool_t;

/**
 * Pre-allocated memory pool for hash table entries
 */
#define MAX_HASH_ENTRIES_IN_POOL 10000

typedef struct {
    order_map_entry_t entries[MAX_HASH_ENTRIES_IN_POOL];
    uint32_t free_list[MAX_HASH_ENTRIES_IN_POOL];
    int free_count;
    
    // Statistics
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
 * Core Data Structures
 * ============================================================================ */

/**
 * Price level - holds orders at a specific price
 */
typedef struct {
    uint32_t price;
    uint32_t total_quantity;  /* Sum of remaining_qty of all orders */
    order_t* orders_head;     /* Doubly-linked list of orders (time priority) */
    order_t* orders_tail;
    bool active;              /* Is this price level in use? */
} price_level_t;

/**
 * Order location for cancellation lookup
 */
typedef struct {
    side_t side;
    uint32_t price;
    order_t* order_ptr;       /* Direct pointer to order in list */
} order_location_t;

/**
 * Hash table entry for order lookup
 */
typedef struct order_map_entry {
    uint64_t key;             /* (user_id << 32) | user_order_id */
    order_location_t location;
    struct order_map_entry* next;  /* For hash collision chaining */
} order_map_entry_t;

/**
 * Simple hash table for order lookup
 */
typedef struct {
    order_map_entry_t* buckets[ORDER_MAP_SIZE];
} order_map_t;

/**
 * Order book structure
 */
typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];

    /* Price levels - fixed arrays */
    price_level_t bids[MAX_PRICE_LEVELS];    /* Sorted descending */
    price_level_t asks[MAX_PRICE_LEVELS];    /* Sorted ascending */

    int num_bid_levels;  /* Number of active bid levels */
    int num_ask_levels;  /* Number of active ask levels */

    /* Order lookup for cancellations */
    order_map_t order_map;

    /* Track previous best bid/ask for TOB change detection */
    uint32_t prev_best_bid_price;
    uint32_t prev_best_bid_qty;
    uint32_t prev_best_ask_price;
    uint32_t prev_best_ask_qty;

    /* Track if sides ever had orders (for elimination messages) */
    bool bid_side_ever_active;
    bool ask_side_ever_active;

    /* Memory pools - no more malloc/free! */
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

/**
 * Initialize order book for a symbol
 * 
 * @param book Order book to initialize
 * @param symbol Symbol name (e.g., "AAPL")
 * @param pools Pre-initialized memory pools
 */
void order_book_init(order_book_t* book, const char* symbol, memory_pools_t* pools);

/**
 * Destroy order book and return all memory to pools
 */
void order_book_destroy(order_book_t* book);

/**
 * Process new order, returns output messages (ack, trades, TOB updates)
 * 
 * @param book Order book
 * @param msg New order message
 * @param client_id Client ID who placed this order (0 for UDP)
 * @param output Output buffer for generated messages
 */
void order_book_add_order(order_book_t* book, 
                          const new_order_msg_t* msg,
                          uint32_t client_id,
                          output_buffer_t* output);

/**
 * Cancel order, returns output messages (cancel ack, TOB updates)
 */
void order_book_cancel_order(order_book_t* book, 
                              uint32_t user_id,
                              uint32_t user_order_id, 
                              output_buffer_t* output);

/**
 * Flush/clear the entire order book
 */
void order_book_flush(order_book_t* book, output_buffer_t* output);

/**
 * Cancel all orders for a specific client (TCP mode)
 * 
 * @param book Order book
 * @param client_id Client ID whose orders should be cancelled
 * @param output Output buffer for cancel acknowledgements
 * @return Number of orders cancelled
 */
size_t order_book_cancel_client_orders(order_book_t* book,
                                       uint32_t client_id,
                                       output_buffer_t* output);

/**
 * Get best bid/ask prices (0 if none)
 */
uint32_t order_book_get_best_bid_price(const order_book_t* book);
uint32_t order_book_get_best_ask_price(const order_book_t* book);

/**
 * Get total quantity at best bid/ask
 */
uint32_t order_book_get_best_bid_quantity(const order_book_t* book);
uint32_t order_book_get_best_ask_quantity(const order_book_t* book);

/* ============================================================================
 * Helper Functions (Inline)
 * ============================================================================ */

/**
 * Initialize output buffer
 */
static inline void output_buffer_init(output_buffer_t* buf) {
    buf->count = 0;
}

/**
 * Add message to output buffer
 */
static inline void output_buffer_add(output_buffer_t* buf, const output_msg_t* msg) {
    if (buf->count < MAX_OUTPUT_MESSAGES) {
        buf->messages[buf->count++] = *msg;
    }
}

/**
 * Combine user_id and user_order_id into single key
 */
static inline uint64_t make_order_key(uint32_t user_id, uint32_t user_order_id) {
    return ((uint64_t)user_id << 32) | user_order_id;
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_ORDER_BOOK_H */

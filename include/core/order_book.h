#ifndef MATCHING_ENGINE_ORDER_BOOK_H
#define MATCHING_ENGINE_ORDER_BOOK_H

#include "core/order.h"
#include "protocol/message_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OrderBook - Single symbol order book with price-time priority
 * 
 * Design decisions:
 * - Fixed array of price levels (10000 slots) - Option B approach
 * - Binary search to find price levels O(log N)
 * - Doubly-linked list for orders at each price (FIFO time priority)
 * - Bids sorted descending (best bid = highest price = highest index)
 * - Asks sorted ascending (best ask = lowest price = lowest index)
 * - Hash table for fast order cancellation lookup
 * 
 * Price level strategy:
 * - Since price range is typically ~100 levels, fixed array is optimal
 * - Better cache locality than tree structures
 * - Simple binary search for insertion
 */

/* Maximum price levels we can handle */
#define MAX_PRICE_LEVELS 10000

/* Maximum orders per price level (for pre-allocation estimate) */
#define TYPICAL_ORDERS_PER_LEVEL 20

/* Maximum output messages from a single operation */
#define MAX_OUTPUT_MESSAGES 1024

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
 * Hash table entry for order lookup (replaces std::unordered_map)
 */
typedef struct order_map_entry {
    uint64_t key;             /* (user_id << 32) | user_order_id */
    order_location_t location;
    struct order_map_entry* next;  /* For hash collision chaining */
} order_map_entry_t;

/**
 * Simple hash table for order lookup
 */
#define ORDER_MAP_SIZE 4096
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

} order_book_t;

/**
 * Output buffer for messages (replaces std::vector<OutputMessage>)
 */
typedef struct {
    output_msg_t messages[MAX_OUTPUT_MESSAGES];
    int count;
} output_buffer_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize order book for a symbol
 */
void order_book_init(order_book_t* book, const char* symbol);

/**
 * Destroy order book and free all memory
 */
void order_book_destroy(order_book_t* book);

/**
 * Process new order, returns output messages (ack, trades, TOB updates)
 */
void order_book_add_order(order_book_t* book, const new_order_msg_t* msg, 
                          output_buffer_t* output);

/**
 * Cancel order, returns output messages (cancel ack, TOB updates)
 */
void order_book_cancel_order(order_book_t* book, uint32_t user_id, 
                              uint32_t user_order_id, output_buffer_t* output);

/**
 * Flush/clear the entire order book
 */
void order_book_flush(order_book_t* book, output_buffer_t* output);

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
 * Helper Functions (Internal)
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

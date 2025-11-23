#ifndef MATCHING_ENGINE_MATCHING_ENGINE_H
#define MATCHING_ENGINE_MATCHING_ENGINE_H

#include "core/order_book.h"
#include "protocol/message_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MatchingEngine - Multi-symbol order book orchestrator
 *
 * Power of 10 Compliant:
 * - Rule 2: All loops bounded
 * - Rule 3: No malloc after init (uses memory pools)
 * - Rule 5: Assertions for invariant checking
 * - Rule 7: Parameter validation
 *
 * Updated for TCP multi-client support:
 * - Tracks client_id with each order for ownership
 * - Supports cancelling all orders for a disconnected client
 * - Passes client_id through to order books
 *
 * Design decisions:
 * - Maintains one OrderBook per symbol
 * - Creates order books on-demand when first order arrives
 * - Routes input messages to appropriate order book
 * - Aggregates output messages from all order books
 * - Uses hash table for symbol -> order book mapping
 * - Tracks order -> symbol mapping for cancellations
 * - Pre-allocated memory pools for zero runtime allocation
 */

#define MAX_SYMBOLS 256
#define SYMBOL_MAP_SIZE 256
#define ORDER_SYMBOL_MAP_SIZE 8192

/* Memory pool sizes for hash table entries */
#define MAX_SYMBOL_MAP_ENTRIES 512
#define MAX_ORDER_SYMBOL_ENTRIES 10000

/* Maximum iterations for hash table traversal (Rule 2) */
#define MAX_HASH_CHAIN_ITERATIONS 100

/* ============================================================================
 * Memory Pools for Hash Table Entries (Rule 3)
 * ============================================================================ */

/**
 * Symbol to order book mapping entry
 */
typedef struct symbol_map_entry {
    char symbol[MAX_SYMBOL_LENGTH];
    order_book_t* book;
    struct symbol_map_entry* next;  /* Hash collision chain */
} symbol_map_entry_t;

/**
 * Order to symbol mapping entry (for cancel operations)
 */
typedef struct order_symbol_entry {
    uint64_t order_key;  /* (user_id << 32) | user_order_id */
    char symbol[MAX_SYMBOL_LENGTH];
    struct order_symbol_entry* next;  /* Hash collision chain */
} order_symbol_entry_t;

/**
 * Memory pool for symbol map entries
 */
typedef struct {
    symbol_map_entry_t entries[MAX_SYMBOL_MAP_ENTRIES];
    uint32_t free_list[MAX_SYMBOL_MAP_ENTRIES];
    int free_count;
    uint32_t total_allocations;
    uint32_t peak_usage;
    uint32_t allocation_failures;
} symbol_map_pool_t;

/**
 * Memory pool for order->symbol entries
 */
typedef struct {
    order_symbol_entry_t entries[MAX_ORDER_SYMBOL_ENTRIES];
    uint32_t free_list[MAX_ORDER_SYMBOL_ENTRIES];
    int free_count;
    uint32_t total_allocations;
    uint32_t peak_usage;
    uint32_t allocation_failures;
} order_symbol_pool_t;

/* ============================================================================
 * Matching Engine Structure
 * ============================================================================ */

/**
 * Matching engine structure
 */
typedef struct {
    /* Symbol -> OrderBook mapping */
    symbol_map_entry_t* symbol_map[SYMBOL_MAP_SIZE];
    
    /* Order -> Symbol mapping (for cancellations) */
    order_symbol_entry_t* order_to_symbol[ORDER_SYMBOL_MAP_SIZE];
    
    /* Pre-allocated order books */
    order_book_t books[MAX_SYMBOLS];
    int num_books;
    
    /* Memory pools - shared reference */
    memory_pools_t* pools;
    
    /* Engine-specific memory pools */
    symbol_map_pool_t symbol_pool;
    order_symbol_pool_t order_symbol_pool;
    
} matching_engine_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize matching engine with memory pools
 * 
 * @param engine Matching engine to initialize
 * @param pools Pre-initialized memory pools (must outlive engine)
 */
void matching_engine_init(matching_engine_t* engine, memory_pools_t* pools);

/**
 * Destroy matching engine and return all memory to pools
 */
void matching_engine_destroy(matching_engine_t* engine);

/**
 * Process input message, returns output messages
 * 
 * @param engine Matching engine
 * @param msg Input message
 * @param client_id Client ID (0 for UDP mode, >0 for TCP client)
 * @param output Output buffer for generated messages
 */
void matching_engine_process_message(matching_engine_t* engine,
                                     const input_msg_t* msg,
                                     uint32_t client_id,
                                     output_buffer_t* output);

/**
 * Process new order
 * 
 * @param engine Matching engine
 * @param msg New order message
 * @param client_id Client ID who placed this order
 * @param output Output buffer
 */
void matching_engine_process_new_order(matching_engine_t* engine,
                                       const new_order_msg_t* msg,
                                       uint32_t client_id,
                                       output_buffer_t* output);

/**
 * Process cancel order
 */
void matching_engine_process_cancel_order(matching_engine_t* engine,
                                          const cancel_msg_t* msg,
                                          output_buffer_t* output);

/**
 * Process flush - clears all order books
 */
void matching_engine_process_flush(matching_engine_t* engine,
                                   output_buffer_t* output);

/**
 * Cancel all orders for a specific client (TCP mode)
 * 
 * Called when a TCP client disconnects. Walks through all order books
 * and cancels orders where order->client_id matches.
 * 
 * @param engine Matching engine
 * @param client_id Client ID to cancel orders for
 * @param output Output buffer for cancel acknowledgements
 * @return Number of orders cancelled
 */
size_t matching_engine_cancel_client_orders(matching_engine_t* engine,
                                            uint32_t client_id,
                                            output_buffer_t* output);

/**
 * Get or create order book for symbol
 */
order_book_t* matching_engine_get_order_book(matching_engine_t* engine,
                                             const char* symbol);

/* ============================================================================
 * Helper Functions (Internal)
 * ============================================================================ */

/**
 * Simple string hash function (djb2)
 */
static inline uint32_t hash_string(const char* str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

/**
 * Hash function for order key
 */
static inline uint32_t hash_order_key(uint64_t key) {
    return (uint32_t)(key ^ (key >> 32));
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_MATCHING_ENGINE_H */

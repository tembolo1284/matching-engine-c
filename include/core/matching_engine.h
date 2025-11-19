#ifndef MATCHING_ENGINE_MATCHING_ENGINE_H
#define MATCHING_ENGINE_MATCHING_ENGINE_H

#include "order_book.h"
#include "message_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MatchingEngine - Multi-symbol order book orchestrator
 * 
 * Design decisions:
 * - Maintains one OrderBook per symbol
 * - Creates order books on-demand when first order arrives
 * - Routes input messages to appropriate order book
 * - Aggregates output messages from all order books
 * - Uses hash table for symbol -> order book mapping
 * - Tracks order -> symbol mapping for cancellations
 */

#define MAX_SYMBOLS 256
#define SYMBOL_MAP_SIZE 256
#define ORDER_SYMBOL_MAP_SIZE 8192

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
 * Matching engine structure
 */
typedef struct {
    /* Symbol -> OrderBook mapping */
    symbol_map_entry_t* symbol_map[SYMBOL_MAP_SIZE];
    
    /* Order -> Symbol mapping (for cancellations) */
    order_symbol_entry_t* order_to_symbol[ORDER_SYMBOL_MAP_SIZE];
    
    /* Pre-allocated order books (to avoid malloc in hot path) */
    order_book_t books[MAX_SYMBOLS];
    int num_books;
} matching_engine_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize matching engine
 */
void matching_engine_init(matching_engine_t* engine);

/**
 * Destroy matching engine and free all memory
 */
void matching_engine_destroy(matching_engine_t* engine);

/**
 * Process input message, returns output messages
 */
void matching_engine_process_message(matching_engine_t* engine, 
                                     const input_msg_t* msg,
                                     output_buffer_t* output);

/**
 * Process new order
 */
void matching_engine_process_new_order(matching_engine_t* engine,
                                       const new_order_msg_t* msg,
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

#ifndef MATCHING_ENGINE_MATCHING_ENGINE_H
#define MATCHING_ENGINE_MATCHING_ENGINE_H

#include "core/order_book.h"
#include "protocol/message_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MatchingEngine - Multi-symbol order book orchestrator
 *
 * Updated for cache optimization:
 * - Open-addressing hash tables (no pointer chasing)
 * - Power-of-2 table sizes for fast masking
 * - Tombstone-based deletion
 *
 * TCP multi-client support:
 * - Tracks client_id with each order for ownership
 * - Supports cancelling all orders for a disconnected client
 */

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

#define MAX_SYMBOLS 64

/*
 * Hash table sizes - MUST be power of 2 for fast masking
 * Symbol map: 512 slots for up to ~256 symbols at 50% load
 * Order-symbol map: 16384 slots for up to ~8192 orders at 50% load
 */
#define SYMBOL_MAP_SIZE 512
#define SYMBOL_MAP_MASK (SYMBOL_MAP_SIZE - 1)

#define ORDER_SYMBOL_MAP_SIZE 8192
#define ORDER_SYMBOL_MAP_MASK (ORDER_SYMBOL_MAP_SIZE - 1)

/* Maximum probe length for open-addressing (Rule 2) */
#define MAX_SYMBOL_PROBE_LENGTH 64
#define MAX_ORDER_SYMBOL_PROBE_LENGTH 128

/* Sentinel values for open-addressing */
#define SYMBOL_SLOT_EMPTY   0   /* symbol[0] == '\0' means empty */
#define ORDER_KEY_EMPTY     0ULL
#define ORDER_KEY_TOMBSTONE UINT64_MAX

/* ============================================================================
 * Open-Addressing Hash Table Structures
 * ============================================================================ */

/**
 * Symbol map slot (symbol → order book index)
 * Empty slot: symbol[0] == '\0'
 */
typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];
    int book_index;  /* Index into books[] array, -1 if empty */
} symbol_map_slot_t;

/**
 * Order-to-symbol map slot (order key → symbol)
 * Used for cancel operations to find which book an order belongs to
 * Empty slot: order_key == 0, Tombstone: order_key == UINT64_MAX
 */
typedef struct {
    uint64_t order_key;  /* (user_id << 32) | user_order_id */
    char symbol[MAX_SYMBOL_LENGTH];
} order_symbol_slot_t;

/**
 * Open-addressing hash table for symbol → order book mapping
 */
typedef struct {
    symbol_map_slot_t slots[SYMBOL_MAP_SIZE];
    uint32_t count;
} symbol_map_t;

/**
 * Open-addressing hash table for order → symbol mapping
 */
typedef struct {
    order_symbol_slot_t slots[ORDER_SYMBOL_MAP_SIZE];
    uint32_t count;
    uint32_t tombstone_count;
} order_symbol_map_t;

/* ============================================================================
 * Matching Engine Structure
 * ============================================================================ */

/**
 * Matching engine structure
 */
typedef struct {
    /* Symbol → OrderBook mapping (open-addressing) */
    symbol_map_t symbol_map;

    /* Order → Symbol mapping for cancellations (open-addressing) */
    order_symbol_map_t order_to_symbol;

    /* Pre-allocated order books */
    order_book_t books[MAX_SYMBOLS];
    int num_books;

    /* Memory pools - shared reference (for order books) */
    memory_pools_t* pools;

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
 * Destroy matching engine
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
 * 
 * Note: This starts an iterative flush. For large order books,
 * call matching_engine_continue_flush() in a loop until it returns true,
 * draining the output buffer between iterations.
 */
void matching_engine_process_flush(matching_engine_t* engine,
                                   output_buffer_t* output);

/**
 * Continue an in-progress flush operation
 * 
 * Call this repeatedly, draining the output buffer between calls,
 * until it returns true (flush complete).
 *
 * @param engine Matching engine
 * @param output Output buffer for generated messages
 * @return true if flush is complete, false if more iterations needed
 */
bool matching_engine_continue_flush(matching_engine_t* engine,
                                    output_buffer_t* output);

/**
 * Check if any flush operation is in progress
 *
 * @param engine Matching engine
 * @return true if a flush is in progress
 */
bool matching_engine_has_flush_in_progress(matching_engine_t* engine);

/**
 * Cancel all orders for a specific client (TCP mode)
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
 * Hash Functions (Inline)
 * ============================================================================ */

/**
 * Hash function for symbol string
 * Uses FNV-1a for good distribution
 */
static inline uint32_t me_hash_symbol(const char* symbol) {
    uint32_t hash = 2166136261u;  /* FNV offset basis */

    for (int i = 0; i < MAX_SYMBOL_LENGTH && symbol[i] != '\0'; i++) {
        hash ^= (uint8_t)symbol[i];
        hash *= 16777619u;  /* FNV prime */
    }

    return hash & SYMBOL_MAP_MASK;
}

/**
 * Hash function for order key
 * Uses multiply-shift for 64-bit keys
 */
static inline uint32_t me_hash_order_key(uint64_t key) {
    const uint64_t GOLDEN_RATIO = 0x9E3779B97F4A7C15ULL;
    key ^= key >> 33;
    key *= GOLDEN_RATIO;
    key ^= key >> 29;
    return (uint32_t)(key & ORDER_SYMBOL_MAP_MASK);
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_MATCHING_ENGINE_H */

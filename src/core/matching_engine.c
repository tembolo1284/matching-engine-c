#include "core/matching_engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Memory Pool Implementation - Symbol Map Entries (Rule 3)
 * ============================================================================ */

/**
 * Initialize symbol map pool
 */
static void symbol_map_pool_init(symbol_map_pool_t* pool) {
    pool->free_count = MAX_SYMBOL_MAP_ENTRIES;
    
    for (int i = 0; i < MAX_SYMBOL_MAP_ENTRIES; i++) {
        pool->free_list[i] = i;
    }
    
    pool->total_allocations = 0;
    pool->peak_usage = 0;
    pool->allocation_failures = 0;
}

/**
 * Allocate symbol map entry from pool
 */
static inline symbol_map_entry_t* symbol_map_entry_alloc(symbol_map_pool_t* pool) {
    #ifdef DEBUG
    if (pool == NULL) {
        fprintf(stderr, "ERROR: NULL pool in symbol_map_entry_alloc\n");
        return NULL;
    }
    #endif
    
    if (pool->free_count == 0) {
        pool->allocation_failures++;
        #ifdef DEBUG
        fprintf(stderr, "ERROR: Symbol map pool exhausted!\n");
        #endif
        return NULL;
    }
    
    pool->free_count--;
    uint32_t index = pool->free_list[pool->free_count];
    
    pool->total_allocations++;
    uint32_t current_usage = MAX_SYMBOL_MAP_ENTRIES - pool->free_count;
    if (current_usage > pool->peak_usage) {
        pool->peak_usage = current_usage;
    }
    
    return &pool->entries[index];
}

/**
 * Free symbol map entry back to pool
 */
static inline void symbol_map_entry_free(symbol_map_pool_t* pool, symbol_map_entry_t* entry) {
    #ifdef DEBUG
    if (pool == NULL || entry == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in symbol_map_entry_free\n");
        return;
    }
    
    if (entry < pool->entries || entry >= pool->entries + MAX_SYMBOL_MAP_ENTRIES) {
        fprintf(stderr, "ERROR: Freeing entry not from pool!\n");
        return;
    }
    
    if (pool->free_count >= MAX_SYMBOL_MAP_ENTRIES) {
        fprintf(stderr, "ERROR: Double-free in symbol map pool!\n");
        return;
    }
    #endif
    
    uint32_t index = (uint32_t)(entry - pool->entries);
    pool->free_list[pool->free_count] = index;
    pool->free_count++;
}

/* ============================================================================
 * Memory Pool Implementation - Order Symbol Entries (Rule 3)
 * ============================================================================ */

/**
 * Initialize order->symbol map pool
 */
static void order_symbol_pool_init(order_symbol_pool_t* pool) {
    pool->free_count = MAX_ORDER_SYMBOL_ENTRIES;
    
    for (int i = 0; i < MAX_ORDER_SYMBOL_ENTRIES; i++) {
        pool->free_list[i] = i;
    }
    
    pool->total_allocations = 0;
    pool->peak_usage = 0;
    pool->allocation_failures = 0;
}

/**
 * Allocate order symbol entry from pool
 */
static inline order_symbol_entry_t* order_symbol_entry_alloc(order_symbol_pool_t* pool) {
    #ifdef DEBUG
    if (pool == NULL) {
        fprintf(stderr, "ERROR: NULL pool in order_symbol_entry_alloc\n");
        return NULL;
    }
    #endif
    
    if (pool->free_count == 0) {
        pool->allocation_failures++;
        #ifdef DEBUG
        fprintf(stderr, "ERROR: Order symbol pool exhausted!\n");
        #endif
        return NULL;
    }
    
    pool->free_count--;
    uint32_t index = pool->free_list[pool->free_count];
    
    pool->total_allocations++;
    uint32_t current_usage = MAX_ORDER_SYMBOL_ENTRIES - pool->free_count;
    if (current_usage > pool->peak_usage) {
        pool->peak_usage = current_usage;
    }
    
    return &pool->entries[index];
}

/**
 * Free order symbol entry back to pool
 */
static inline void order_symbol_entry_free(order_symbol_pool_t* pool, order_symbol_entry_t* entry) {
    #ifdef DEBUG
    if (pool == NULL || entry == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in order_symbol_entry_free\n");
        return;
    }
    
    if (entry < pool->entries || entry >= pool->entries + MAX_ORDER_SYMBOL_ENTRIES) {
        fprintf(stderr, "ERROR: Freeing entry not from pool!\n");
        return;
    }
    
    if (pool->free_count >= MAX_ORDER_SYMBOL_ENTRIES) {
        fprintf(stderr, "ERROR: Double-free in order symbol pool!\n");
        return;
    }
    #endif
    
    uint32_t index = (uint32_t)(entry - pool->entries);
    pool->free_list[pool->free_count] = index;
    pool->free_count++;
}

/* ============================================================================
 * Helper Functions - Hash Tables (Now using memory pools)
 * ============================================================================ */

/**
 * Hash function for symbol string (djb2)
 */
static uint32_t hash_symbol(const char* symbol) {
    uint32_t hash = 5381;
    int c;
    
    // Rule 2: Bounded loop
    int count = 0;
    while ((c = *symbol++) && count < MAX_SYMBOL_LENGTH) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
        count++;
    }
    return hash % SYMBOL_MAP_SIZE;
}

/**
 * Insert into symbol map (now uses pool)
 */
static void symbol_map_insert(matching_engine_t* engine, const char* symbol, order_book_t* book) {
    uint32_t hash = hash_symbol(symbol);

    // Allocate from pool instead of malloc
    symbol_map_entry_t* entry = symbol_map_entry_alloc(&engine->symbol_pool);
    
    // Rule 7: Check allocation
    if (entry == NULL) {
        #ifdef DEBUG
        fprintf(stderr, "ERROR: Failed to allocate symbol map entry\n");
        #endif
        return;
    }
    
    strncpy(entry->symbol, symbol, MAX_SYMBOL_LENGTH - 1);
    entry->symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
    entry->book = book;
    entry->next = engine->symbol_map[hash];

    engine->symbol_map[hash] = entry;
}

/**
 * Find in symbol map
 */
static symbol_map_entry_t* symbol_map_find(matching_engine_t* engine, const char* symbol) {
    uint32_t hash = hash_symbol(symbol);
    symbol_map_entry_t* entry = engine->symbol_map[hash];

    // Rule 2: Bounded loop
    int count = 0;
    while (entry != NULL && count < MAX_HASH_CHAIN_ITERATIONS) {
        if (strncmp(entry->symbol, symbol, MAX_SYMBOL_LENGTH) == 0) {
            return entry;
        }
        entry = entry->next;
        count++;
    }

    return NULL;
}

/**
 * Insert into order->symbol map (now uses pool)
 */
static void order_symbol_map_insert(matching_engine_t* engine, uint64_t order_key, const char* symbol) {
    uint32_t hash = hash_order_key(order_key) % ORDER_SYMBOL_MAP_SIZE;

    // Allocate from pool instead of malloc
    order_symbol_entry_t* entry = order_symbol_entry_alloc(&engine->order_symbol_pool);
    
    // Rule 7: Check allocation
    if (entry == NULL) {
        #ifdef DEBUG
        fprintf(stderr, "ERROR: Failed to allocate order symbol entry\n");
        #endif
        return;
    }
    
    entry->order_key = order_key;
    strncpy(entry->symbol, symbol, MAX_SYMBOL_LENGTH - 1);
    entry->symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
    entry->next = engine->order_to_symbol[hash];

    engine->order_to_symbol[hash] = entry;
}

/**
 * Find in order->symbol map
 */
static order_symbol_entry_t* order_symbol_map_find(matching_engine_t* engine, uint64_t order_key) {
    uint32_t hash = hash_order_key(order_key) % ORDER_SYMBOL_MAP_SIZE;
    order_symbol_entry_t* entry = engine->order_to_symbol[hash];

    // Rule 2: Bounded loop
    int count = 0;
    while (entry != NULL && count < MAX_HASH_CHAIN_ITERATIONS) {
        if (entry->order_key == order_key) {
            return entry;
        }
        entry = entry->next;
        count++;
    }

    return NULL;
}

/**
 * Remove from order->symbol map (now uses pool)
 */
static void order_symbol_map_remove(matching_engine_t* engine, uint64_t order_key) {
    uint32_t hash = hash_order_key(order_key) % ORDER_SYMBOL_MAP_SIZE;
    order_symbol_entry_t* entry = engine->order_to_symbol[hash];
    order_symbol_entry_t* prev = NULL;

    // Rule 2: Bounded loop
    int count = 0;
    while (entry != NULL && count < MAX_HASH_CHAIN_ITERATIONS) {
        if (entry->order_key == order_key) {
            if (prev != NULL) {
                prev->next = entry->next;
            } else {
                engine->order_to_symbol[hash] = entry->next;
            }
            order_symbol_entry_free(&engine->order_symbol_pool, entry);  // ← Use pool
            return;
        }
        prev = entry;
        entry = entry->next;
        count++;
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/**
 * Initialize matching engine with memory pools
 */
void matching_engine_init(matching_engine_t* engine, memory_pools_t* pools) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (engine == NULL || pools == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in matching_engine_init\n");
        return;
    }
    #endif

    // Initialize symbol map
    memset(engine->symbol_map, 0, sizeof(engine->symbol_map));

    // Initialize order->symbol map
    memset(engine->order_to_symbol, 0, sizeof(engine->order_to_symbol));

    // Initialize order books array
    memset(engine->books, 0, sizeof(engine->books));
    engine->num_books = 0;
    
    // Store reference to shared pools
    engine->pools = pools;
    
    // Initialize engine-specific pools
    symbol_map_pool_init(&engine->symbol_pool);
    order_symbol_pool_init(&engine->order_symbol_pool);
}

/**
 * Destroy matching engine and return all memory to pools
 */
void matching_engine_destroy(matching_engine_t* engine) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (engine == NULL) {
        fprintf(stderr, "ERROR: NULL engine in matching_engine_destroy\n");
        return;
    }
    #endif

    // Destroy all order books
    for (int i = 0; i < engine->num_books; i++) {
        order_book_destroy(&engine->books[i]);
    }

    // Free symbol map (now uses pool)
    for (int i = 0; i < SYMBOL_MAP_SIZE; i++) {
        symbol_map_entry_t* entry = engine->symbol_map[i];
        
        // Rule 2: Bounded loop
        int count = 0;
        while (entry != NULL && count < MAX_HASH_CHAIN_ITERATIONS) {
            symbol_map_entry_t* next = entry->next;
            symbol_map_entry_free(&engine->symbol_pool, entry);  // ← Use pool
            entry = next;
            count++;
        }
    }

    // Free order->symbol map (now uses pool)
    for (int i = 0; i < ORDER_SYMBOL_MAP_SIZE; i++) {
        order_symbol_entry_t* entry = engine->order_to_symbol[i];
        
        // Rule 2: Bounded loop
        int count = 0;
        while (entry != NULL && count < MAX_HASH_CHAIN_ITERATIONS) {
            order_symbol_entry_t* next = entry->next;
            order_symbol_entry_free(&engine->order_symbol_pool, entry);  // ← Use pool
            entry = next;
            count++;
        }
    }
    
    engine->pools = NULL;
}

/**
 * Get or create order book for symbol
 */
order_book_t* matching_engine_get_order_book(matching_engine_t* engine, const char* symbol) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (engine == NULL || symbol == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in matching_engine_get_order_book\n");
        return NULL;
    }
    #endif

    // Check if order book already exists
    symbol_map_entry_t* entry = symbol_map_find(engine, symbol);

    if (entry != NULL) {
        return entry->book;
    }

    // Create new order book
    if (engine->num_books >= MAX_SYMBOLS) {
        fprintf(stderr, "ERROR: Maximum number of symbols (%d) reached\n", MAX_SYMBOLS);
        return NULL;
    }

    order_book_t* book = &engine->books[engine->num_books];
    order_book_init(book, symbol, engine->pools);  // ← CRITICAL: Pass pools!
    engine->num_books++;

    // Add to symbol map
    symbol_map_insert(engine, symbol, book);

    return book;
}

/**
 * Process new order (WITH CLIENT_ID and memory pools)
 */
void matching_engine_process_new_order(matching_engine_t* engine, 
                                       const new_order_msg_t* msg,
                                       uint32_t client_id,
                                       output_buffer_t* output) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (engine == NULL || msg == NULL || output == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in matching_engine_process_new_order\n");
        return;
    }
    #endif

    // Get or create order book for this symbol
    order_book_t* book = matching_engine_get_order_book(engine, msg->symbol);

    if (book == NULL) {
        // Failed to get/create order book - send ack anyway
        output_msg_t ack = make_ack_msg(msg->symbol, msg->user_id, msg->user_order_id);
        output_buffer_add(output, &ack);
        return;
    }

    // Track order location for future cancellations
    uint64_t order_key = make_order_key(msg->user_id, msg->user_order_id);
    order_symbol_map_insert(engine, order_key, msg->symbol);

    // Process the order with client_id
    order_book_add_order(book, msg, client_id, output);
}

/**
 * Process cancel order
 */
void matching_engine_process_cancel_order(matching_engine_t* engine, 
                                          const cancel_msg_t* msg,
                                          output_buffer_t* output) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (engine == NULL || msg == NULL || output == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in matching_engine_process_cancel_order\n");
        return;
    }
    #endif

    // Find which symbol this order belongs to
    uint64_t order_key = make_order_key(msg->user_id, msg->user_order_id);
    order_symbol_entry_t* entry = order_symbol_map_find(engine, order_key);

    if (entry == NULL) {
        // Order not found - send cancel ack anyway
        output_msg_t cancel_ack = make_cancel_ack_msg(msg->symbol, msg->user_id, msg->user_order_id);
        output_buffer_add(output, &cancel_ack);
        return;
    }

    // Get the order book
    symbol_map_entry_t* book_entry = symbol_map_find(engine, entry->symbol);

    if (book_entry == NULL) {
        // Order book doesn't exist - send cancel ack anyway
        output_msg_t cancel_ack = make_cancel_ack_msg(msg->symbol, msg->user_id, msg->user_order_id);
        output_buffer_add(output, &cancel_ack);
        order_symbol_map_remove(engine, order_key);
        return;
    }

    // Cancel the order
    order_book_cancel_order(book_entry->book, msg->user_id, msg->user_order_id, output);

    // Remove from tracking map
    order_symbol_map_remove(engine, order_key);
}

/**
 * Process flush - clears all order books
 */
void matching_engine_process_flush(matching_engine_t* engine, output_buffer_t* output) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (engine == NULL || output == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in matching_engine_process_flush\n");
        return;
    }
    #endif

    // Flush all order books
    for (int i = 0; i < engine->num_books; i++) {
        order_book_flush(&engine->books[i], output);
    }

    // Clear order->symbol map (now uses pool)
    for (int i = 0; i < ORDER_SYMBOL_MAP_SIZE; i++) {
        order_symbol_entry_t* entry = engine->order_to_symbol[i];
        
        // Rule 2: Bounded loop
        int count = 0;
        while (entry != NULL && count < MAX_HASH_CHAIN_ITERATIONS) {
            order_symbol_entry_t* next = entry->next;
            order_symbol_entry_free(&engine->order_symbol_pool, entry);  // ← Use pool
            entry = next;
            count++;
        }
        engine->order_to_symbol[i] = NULL;
    }
}

/**
 * Cancel all orders for a specific client (TCP mode)
 */
size_t matching_engine_cancel_client_orders(matching_engine_t* engine,
                                            uint32_t client_id,
                                            output_buffer_t* output) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (engine == NULL || output == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in matching_engine_cancel_client_orders\n");
        return 0;
    }
    #endif

    size_t total_cancelled = 0;
    
    // Walk through all order books and cancel orders for this client
    // Rule 2: Bounded by MAX_SYMBOLS
    for (int i = 0; i < engine->num_books && i < MAX_SYMBOLS; i++) {
        size_t cancelled = order_book_cancel_client_orders(&engine->books[i], 
                                                           client_id, 
                                                           output);
        total_cancelled += cancelled;
    }
    
    fprintf(stderr, "[Matching Engine] Cancelled %zu orders for client %u\n", 
            total_cancelled, client_id);
    
    return total_cancelled;
}

/**
 * Process input message (WITH CLIENT_ID)
 */
void matching_engine_process_message(matching_engine_t* engine, 
                                     const input_msg_t* msg,
                                     uint32_t client_id,
                                     output_buffer_t* output) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (engine == NULL || msg == NULL || output == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in matching_engine_process_message\n");
        return;
    }
    #endif

    switch (msg->type) {
        case INPUT_MSG_NEW_ORDER:
            matching_engine_process_new_order(engine, &msg->data.new_order, client_id, output);
            break;

        case INPUT_MSG_CANCEL:
            matching_engine_process_cancel_order(engine, &msg->data.cancel, output);
            break;

        case INPUT_MSG_FLUSH:
            matching_engine_process_flush(engine, output);
            break;

        default:
            #ifdef DEBUG
            fprintf(stderr, "WARNING: Unknown message type: %d\n", msg->type);
            #endif
            break;
    }
}

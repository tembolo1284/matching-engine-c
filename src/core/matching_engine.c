#include "core/matching_engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ============================================================================
 * Symbol Map - Open-Addressing Hash Table
 * ============================================================================ */

/**
 * Initialize symbol map
 */
static void symbol_map_init(symbol_map_t* map) {
    assert(map != NULL && "NULL map in symbol_map_init");
    
    /* Clear all slots - symbol[0] == '\0' means empty */
    memset(map->slots, 0, sizeof(map->slots));
    map->count = 0;
}

/**
 * Check if symbol slot is empty
 */
static inline bool symbol_slot_is_empty(const symbol_map_slot_t* slot) {
    return slot->symbol[0] == '\0';
}

/**
 * Find symbol in map using linear probing
 * Returns pointer to slot if found, NULL if not found
 */
static symbol_map_slot_t* symbol_map_find(symbol_map_t* map, const char* symbol) {
    assert(map != NULL && "NULL map in symbol_map_find");
    assert(symbol != NULL && "NULL symbol in symbol_map_find");
    
    uint32_t hash = me_hash_symbol(symbol);
    
    /* Rule 2: Bounded probe sequence */
    for (int i = 0; i < MAX_SYMBOL_PROBE_LENGTH; i++) {
        uint32_t idx = (hash + (uint32_t)i) & SYMBOL_MAP_MASK;
        symbol_map_slot_t* slot = &map->slots[idx];
        
        /* Found matching symbol */
        if (strncmp(slot->symbol, symbol, MAX_SYMBOL_LENGTH) == 0) {
            return slot;
        }
        
        /* Hit empty slot - symbol not found */
        if (symbol_slot_is_empty(slot)) {
            return NULL;
        }
    }
    
    return NULL;
}

/**
 * Insert symbol into map
 * Returns pointer to slot on success, NULL on failure
 */
static symbol_map_slot_t* symbol_map_insert(symbol_map_t* map, 
                                            const char* symbol, 
                                            int book_index) {
    assert(map != NULL && "NULL map in symbol_map_insert");
    assert(symbol != NULL && "NULL symbol in symbol_map_insert");
    assert(symbol[0] != '\0' && "Empty symbol in symbol_map_insert");
    
    uint32_t hash = me_hash_symbol(symbol);
    
    /* Rule 2: Bounded probe sequence */
    for (int i = 0; i < MAX_SYMBOL_PROBE_LENGTH; i++) {
        uint32_t idx = (hash + (uint32_t)i) & SYMBOL_MAP_MASK;
        symbol_map_slot_t* slot = &map->slots[idx];
        
        /* Found empty slot - insert here */
        if (symbol_slot_is_empty(slot)) {
            strncpy(slot->symbol, symbol, MAX_SYMBOL_LENGTH - 1);
            slot->symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
            slot->book_index = book_index;
            map->count++;
            return slot;
        }
        
        /* Symbol already exists - update (shouldn't happen normally) */
        if (strncmp(slot->symbol, symbol, MAX_SYMBOL_LENGTH) == 0) {
            slot->book_index = book_index;
            return slot;
        }
    }
    
    /* Table full or too many collisions */
    #ifdef DEBUG
    fprintf(stderr, "ERROR: Symbol map probe limit exceeded (count=%u)\n", map->count);
    #endif
    return NULL;
}

/* ============================================================================
 * Order-Symbol Map - Open-Addressing Hash Table
 * ============================================================================ */

/**
 * Initialize order-symbol map
 */
static void order_symbol_map_init(order_symbol_map_t* map) {
    assert(map != NULL && "NULL map in order_symbol_map_init");
    
    memset(map->slots, 0, sizeof(map->slots));
    map->count = 0;
    map->tombstone_count = 0;
}

/**
 * Insert into order-symbol map
 */
static bool order_symbol_map_insert(order_symbol_map_t* map, 
                                    uint64_t order_key, 
                                    const char* symbol) {
    assert(map != NULL && "NULL map in order_symbol_map_insert");
    assert(symbol != NULL && "NULL symbol in order_symbol_map_insert");
    assert(order_key != ORDER_KEY_EMPTY && "Cannot insert empty key");
    assert(order_key != ORDER_KEY_TOMBSTONE && "Cannot insert tombstone key");
    
    uint32_t hash = me_hash_order_key(order_key);
    
    /* Rule 2: Bounded probe sequence */
    for (int i = 0; i < MAX_ORDER_SYMBOL_PROBE_LENGTH; i++) {
        uint32_t idx = (hash + (uint32_t)i) & ORDER_SYMBOL_MAP_MASK;
        order_symbol_slot_t* slot = &map->slots[idx];
        
        /* Found empty or tombstone slot - insert here */
        if (slot->order_key == ORDER_KEY_EMPTY || 
            slot->order_key == ORDER_KEY_TOMBSTONE) {
            
            if (slot->order_key == ORDER_KEY_TOMBSTONE) {
                map->tombstone_count--;
            }
            
            slot->order_key = order_key;
            size_t len = strlen(symbol);
            if (len >= MAX_SYMBOL_LENGTH - 1) len = MAX_SYMBOL_LENGTH - 2;
            memcpy(slot->symbol, symbol, len);
            slot->symbol[len] = '\0';
            map->count++;
            return true;
        }
        
        /* Key already exists - update symbol (shouldn't happen) */
        if (slot->order_key == order_key) {
            size_t len = strlen(symbol);
            if (len >= MAX_SYMBOL_LENGTH - 1) len = MAX_SYMBOL_LENGTH - 2;
            memcpy(slot->symbol, symbol, len);
            slot->symbol[len] = '\0';
            return true;
        }
    }
    
    #ifdef DEBUG
    fprintf(stderr, "ERROR: Order-symbol map probe limit exceeded (count=%u)\n", map->count);
    #endif
    return false;
}

/**
 * Find in order-symbol map
 */
static order_symbol_slot_t* order_symbol_map_find(order_symbol_map_t* map, 
                                                   uint64_t order_key) {
    assert(map != NULL && "NULL map in order_symbol_map_find");
    
    if (order_key == ORDER_KEY_EMPTY || order_key == ORDER_KEY_TOMBSTONE) {
        return NULL;
    }
    
    uint32_t hash = me_hash_order_key(order_key);
    
    /* Rule 2: Bounded probe sequence */
    for (int i = 0; i < MAX_ORDER_SYMBOL_PROBE_LENGTH; i++) {
        uint32_t idx = (hash + (uint32_t)i) & ORDER_SYMBOL_MAP_MASK;
        order_symbol_slot_t* slot = &map->slots[idx];
        
        /* Found it */
        if (slot->order_key == order_key) {
            return slot;
        }
        
        /* Hit empty slot - key doesn't exist */
        if (slot->order_key == ORDER_KEY_EMPTY) {
            return NULL;
        }
        
        /* Tombstone - keep probing */
    }
    
    return NULL;
}

/**
 * Remove from order-symbol map using tombstone
 */
static bool order_symbol_map_remove(order_symbol_map_t* map, uint64_t order_key) {
    assert(map != NULL && "NULL map in order_symbol_map_remove");
    
    order_symbol_slot_t* slot = order_symbol_map_find(map, order_key);
    
    if (slot != NULL) {
        slot->order_key = ORDER_KEY_TOMBSTONE;
        map->count--;
        map->tombstone_count++;
        return true;
    }
    
    return false;
}

/**
 * Clear entire order-symbol map
 */
static void order_symbol_map_clear(order_symbol_map_t* map) {
    assert(map != NULL && "NULL map in order_symbol_map_clear");
    
    memset(map->slots, 0, sizeof(map->slots));
    map->count = 0;
    map->tombstone_count = 0;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/**
 * Initialize matching engine with memory pools
 */
void matching_engine_init(matching_engine_t* engine, memory_pools_t* pools) {
    assert(engine != NULL && "NULL engine in matching_engine_init");
    assert(pools != NULL && "NULL pools in matching_engine_init");

    /* Initialize symbol map (open-addressing) */
    symbol_map_init(&engine->symbol_map);

    /* Initialize order->symbol map (open-addressing) */
    order_symbol_map_init(&engine->order_to_symbol);

    /* Initialize order books array */
    memset(engine->books, 0, sizeof(engine->books));
    engine->num_books = 0;
    
    /* Store reference to shared pools */
    engine->pools = pools;
}

/**
 * Destroy matching engine
 */
void matching_engine_destroy(matching_engine_t* engine) {
    assert(engine != NULL && "NULL engine in matching_engine_destroy");

    /* Destroy all order books */
    for (int i = 0; i < engine->num_books && i < MAX_SYMBOLS; i++) {
        order_book_destroy(&engine->books[i]);
    }
    
    /* No memory to free - open-addressing uses inline arrays */
    engine->pools = NULL;
}

/**
 * Get or create order book for symbol
 */
order_book_t* matching_engine_get_order_book(matching_engine_t* engine, 
                                              const char* symbol) {
    assert(engine != NULL && "NULL engine in matching_engine_get_order_book");
    assert(symbol != NULL && "NULL symbol in matching_engine_get_order_book");

    /* Check if order book already exists */
    symbol_map_slot_t* slot = symbol_map_find(&engine->symbol_map, symbol);

    if (slot != NULL) {
        assert(slot->book_index >= 0 && slot->book_index < engine->num_books);
        return &engine->books[slot->book_index];
    }

    /* Create new order book */
    if (engine->num_books >= MAX_SYMBOLS) {
        fprintf(stderr, "ERROR: Maximum number of symbols (%d) reached\n", MAX_SYMBOLS);
        return NULL;
    }

    int book_index = engine->num_books;
    order_book_t* book = &engine->books[book_index];
    order_book_init(book, symbol, engine->pools);
    engine->num_books++;

    /* Add to symbol map */
    symbol_map_slot_t* new_slot = symbol_map_insert(&engine->symbol_map, symbol, book_index);
    if (new_slot == NULL) {
        /* Failed to insert - rollback */
        engine->num_books--;
        return NULL;
    }

    return book;
}

/**
 * Process new order
 */
void matching_engine_process_new_order(matching_engine_t* engine, 
                                       const new_order_msg_t* msg,
                                       uint32_t client_id,
                                       output_buffer_t* output) {
    assert(engine != NULL && "NULL engine in matching_engine_process_new_order");
    assert(msg != NULL && "NULL msg in matching_engine_process_new_order");
    assert(output != NULL && "NULL output in matching_engine_process_new_order");

    /* Get or create order book for this symbol */
    order_book_t* book = matching_engine_get_order_book(engine, msg->symbol);

    if (book == NULL) {
        /* Failed to get/create order book - send ack anyway */
        output_msg_t ack = make_ack_msg(msg->symbol, msg->user_id, msg->user_order_id);
        output_buffer_add(output, &ack);
        return;
    }

    /* Track order location for future cancellations */
    uint64_t order_key = make_order_key(msg->user_id, msg->user_order_id);
    order_symbol_map_insert(&engine->order_to_symbol, order_key, msg->symbol);

    /* Process the order with client_id */
    order_book_add_order(book, msg, client_id, output);
}

/**
 * Process cancel order
 */
void matching_engine_process_cancel_order(matching_engine_t* engine, 
                                          const cancel_msg_t* msg,
                                          output_buffer_t* output) {
    assert(engine != NULL && "NULL engine in matching_engine_process_cancel_order");
    assert(msg != NULL && "NULL msg in matching_engine_process_cancel_order");
    assert(output != NULL && "NULL output in matching_engine_process_cancel_order");

    /* Find which symbol this order belongs to */
    uint64_t order_key = make_order_key(msg->user_id, msg->user_order_id);
    order_symbol_slot_t* entry = order_symbol_map_find(&engine->order_to_symbol, order_key);

    if (entry == NULL) {
        /* Order not found - send cancel ack anyway */
        output_msg_t cancel_ack = make_cancel_ack_msg(msg->symbol, msg->user_id, msg->user_order_id);
        output_buffer_add(output, &cancel_ack);
        return;
    }

    /* Get the order book */
    symbol_map_slot_t* book_slot = symbol_map_find(&engine->symbol_map, entry->symbol);

    if (book_slot == NULL) {
        /* Order book doesn't exist - send cancel ack anyway */
        output_msg_t cancel_ack = make_cancel_ack_msg(msg->symbol, msg->user_id, msg->user_order_id);
        output_buffer_add(output, &cancel_ack);
        order_symbol_map_remove(&engine->order_to_symbol, order_key);
        return;
    }

    /* Cancel the order */
    assert(book_slot->book_index >= 0 && book_slot->book_index < engine->num_books);
    order_book_cancel_order(&engine->books[book_slot->book_index], 
                           msg->user_id, msg->user_order_id, output);

    /* Remove from tracking map */
    order_symbol_map_remove(&engine->order_to_symbol, order_key);
}

/**
 * Process flush - clears all order books
 */
void matching_engine_process_flush(matching_engine_t* engine, output_buffer_t* output) {
    assert(engine != NULL && "NULL engine in matching_engine_process_flush");
    assert(output != NULL && "NULL output in matching_engine_process_flush");

    /* Flush all order books */
    for (int i = 0; i < engine->num_books && i < MAX_SYMBOLS; i++) {
        order_book_flush(&engine->books[i], output);
    }

    /* Clear order->symbol map */
    order_symbol_map_clear(&engine->order_to_symbol);
}

/**
 * Cancel all orders for a specific client (TCP mode)
 */
size_t matching_engine_cancel_client_orders(matching_engine_t* engine,
                                            uint32_t client_id,
                                            output_buffer_t* output) {
    assert(engine != NULL && "NULL engine in matching_engine_cancel_client_orders");
    assert(output != NULL && "NULL output in matching_engine_cancel_client_orders");

    size_t total_cancelled = 0;
    
    /* Walk through all order books and cancel orders for this client */
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
 * Process input message
 */
void matching_engine_process_message(matching_engine_t* engine, 
                                     const input_msg_t* msg,
                                     uint32_t client_id,
                                     output_buffer_t* output) {
    assert(engine != NULL && "NULL engine in matching_engine_process_message");
    assert(msg != NULL && "NULL msg in matching_engine_process_message");
    assert(output != NULL && "NULL output in matching_engine_process_message");

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

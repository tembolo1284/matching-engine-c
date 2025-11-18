#include "matching_engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Helper Functions - Hash Tables
 * ============================================================================ */

/**
 * Hash function for symbol string (djb2)
 */
static uint32_t hash_symbol(const char* symbol) {
    uint32_t hash = 5381;
    int c;
    while ((c = *symbol++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash % SYMBOL_MAP_SIZE;
}

/**
 * Insert into symbol map
 */
static void symbol_map_insert(matching_engine_t* engine, const char* symbol, order_book_t* book) {
    uint32_t hash = hash_symbol(symbol);
    
    symbol_map_entry_t* entry = (symbol_map_entry_t*)malloc(sizeof(symbol_map_entry_t));
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
    
    while (entry != NULL) {
        if (strncmp(entry->symbol, symbol, MAX_SYMBOL_LENGTH) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

/**
 * Insert into order->symbol map
 */
static void order_symbol_map_insert(matching_engine_t* engine, uint64_t order_key, const char* symbol) {
    uint32_t hash = hash_order_key(order_key) % ORDER_SYMBOL_MAP_SIZE;
    
    order_symbol_entry_t* entry = (order_symbol_entry_t*)malloc(sizeof(order_symbol_entry_t));
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
    
    while (entry != NULL) {
        if (entry->order_key == order_key) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

/**
 * Remove from order->symbol map
 */
static void order_symbol_map_remove(matching_engine_t* engine, uint64_t order_key) {
    uint32_t hash = hash_order_key(order_key) % ORDER_SYMBOL_MAP_SIZE;
    order_symbol_entry_t* entry = engine->order_to_symbol[hash];
    order_symbol_entry_t* prev = NULL;
    
    while (entry != NULL) {
        if (entry->order_key == order_key) {
            if (prev != NULL) {
                prev->next = entry->next;
            } else {
                engine->order_to_symbol[hash] = entry->next;
            }
            free(entry);
            return;
        }
        prev = entry;
        entry = entry->next;
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/**
 * Initialize matching engine
 */
void matching_engine_init(matching_engine_t* engine) {
    // Initialize symbol map
    memset(engine->symbol_map, 0, sizeof(engine->symbol_map));
    
    // Initialize order->symbol map
    memset(engine->order_to_symbol, 0, sizeof(engine->order_to_symbol));
    
    // Initialize order books array
    memset(engine->books, 0, sizeof(engine->books));
    engine->num_books = 0;
}

/**
 * Destroy matching engine and free all memory
 */
void matching_engine_destroy(matching_engine_t* engine) {
    // Destroy all order books
    for (int i = 0; i < engine->num_books; i++) {
        order_book_destroy(&engine->books[i]);
    }
    
    // Free symbol map
    for (int i = 0; i < SYMBOL_MAP_SIZE; i++) {
        symbol_map_entry_t* entry = engine->symbol_map[i];
        while (entry != NULL) {
            symbol_map_entry_t* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    
    // Free order->symbol map
    for (int i = 0; i < ORDER_SYMBOL_MAP_SIZE; i++) {
        order_symbol_entry_t* entry = engine->order_to_symbol[i];
        while (entry != NULL) {
            order_symbol_entry_t* next = entry->next;
            free(entry);
            entry = next;
        }
    }
}

/**
 * Get or create order book for symbol
 */
order_book_t* matching_engine_get_order_book(matching_engine_t* engine, const char* symbol) {
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
    order_book_init(book, symbol);
    engine->num_books++;
    
    // Add to symbol map
    symbol_map_insert(engine, symbol, book);
    
    return book;
}

/**
 * Process new order
 */
void matching_engine_process_new_order(matching_engine_t* engine, const new_order_msg_t* msg,
                                       output_buffer_t* output) {
    // Get or create order book for this symbol
    order_book_t* book = matching_engine_get_order_book(engine, msg->symbol);
    
    if (book == NULL) {
        // Failed to get/create order book - just send ack
        output_msg_t ack = make_ack_msg(msg->user_id, msg->user_order_id);
        output_buffer_add(output, &ack);
        return;
    }
    
    // Track order location for future cancellations
    uint64_t order_key = make_order_key(msg->user_id, msg->user_order_id);
    order_symbol_map_insert(engine, order_key, msg->symbol);
    
    // Process the order
    order_book_add_order(book, msg, output);
}

/**
 * Process cancel order
 */
void matching_engine_process_cancel_order(matching_engine_t* engine, const cancel_msg_t* msg,
                                          output_buffer_t* output) {
    // Find which symbol this order belongs to
    uint64_t order_key = make_order_key(msg->user_id, msg->user_order_id);
    order_symbol_entry_t* entry = order_symbol_map_find(engine, order_key);
    
    if (entry == NULL) {
        // Order not found - send cancel ack anyway
        output_msg_t cancel_ack = make_cancel_ack_msg(msg->user_id, msg->user_order_id);
        output_buffer_add(output, &cancel_ack);
        return;
    }
    
    // Get the order book
    symbol_map_entry_t* book_entry = symbol_map_find(engine, entry->symbol);
    
    if (book_entry == NULL) {
        // Order book doesn't exist - send cancel ack anyway
        output_msg_t cancel_ack = make_cancel_ack_msg(msg->user_id, msg->user_order_id);
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
    (void)output;  // Unused parameter
    
    // Flush all order books
    for (int i = 0; i < engine->num_books; i++) {
        order_book_flush(&engine->books[i]);
    }
    
    // Clear order->symbol map
    for (int i = 0; i < ORDER_SYMBOL_MAP_SIZE; i++) {
        order_symbol_entry_t* entry = engine->order_to_symbol[i];
        while (entry != NULL) {
            order_symbol_entry_t* next = entry->next;
            free(entry);
            entry = next;
        }
        engine->order_to_symbol[i] = NULL;
    }
}

/**
 * Process input message
 */
void matching_engine_process_message(matching_engine_t* engine, const input_msg_t* msg,
                                     output_buffer_t* output) {
    switch (msg->type) {
        case INPUT_MSG_NEW_ORDER:
            matching_engine_process_new_order(engine, &msg->data.new_order, output);
            break;
        
        case INPUT_MSG_CANCEL:
            matching_engine_process_cancel_order(engine, &msg->data.cancel, output);
            break;
        
        case INPUT_MSG_FLUSH:
            matching_engine_process_flush(engine, output);
            break;
        
        default:
            break;
    }
}

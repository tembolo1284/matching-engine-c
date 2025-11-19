#include "order_book.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Helper Functions - Doubly-Linked List Operations
 * ============================================================================ */

/**
 * Add order to end of list (FIFO for time priority)
 */
static void list_append(order_t** head, order_t** tail, order_t* order) {
    order->next = NULL;
    order->prev = *tail;
    
    if (*tail != NULL) {
        (*tail)->next = order;
    }
    
    *tail = order;
    
    if (*head == NULL) {
        *head = order;
    }
}

/**
 * Remove order from list
 */
static void list_remove(order_t** head, order_t** tail, order_t* order) {
    if (order->prev != NULL) {
        order->prev->next = order->next;
    } else {
        *head = order->next;
    }
    
    if (order->next != NULL) {
        order->next->prev = order->prev;
    } else {
        *tail = order->prev;
    }
    
    order->next = NULL;
    order->prev = NULL;
}

/**
 * Free all orders in a list
 */
static void list_free_all(order_t* head) {
    order_t* current = head;
    while (current != NULL) {
        order_t* next = current->next;
        free(current);
        current = next;
    }
}

/* ============================================================================
 * Helper Functions - Hash Table Operations (for order lookup)
 * ============================================================================ */

/**
 * Hash function for order key
 */
static uint32_t hash_order_key(uint64_t key) {
    return (uint32_t)(key % ORDER_MAP_SIZE);
}

/**
 * Insert into order map
 */
static void order_map_insert(order_map_t* map, uint64_t key, const order_location_t* location) {
    uint32_t hash = hash_order_key(key);
    
    // Create new entry
    order_map_entry_t* entry = (order_map_entry_t*)malloc(sizeof(order_map_entry_t));
    entry->key = key;
    entry->location = *location;
    entry->next = map->buckets[hash];
    
    // Insert at head of bucket chain
    map->buckets[hash] = entry;
}

/**
 * Find in order map
 */
static order_map_entry_t* order_map_find(order_map_t* map, uint64_t key) {
    uint32_t hash = hash_order_key(key);
    order_map_entry_t* entry = map->buckets[hash];
    
    while (entry != NULL) {
        if (entry->key == key) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

/**
 * Remove from order map
 */
static void order_map_remove(order_map_t* map, uint64_t key) {
    uint32_t hash = hash_order_key(key);
    order_map_entry_t* entry = map->buckets[hash];
    order_map_entry_t* prev = NULL;
    
    while (entry != NULL) {
        if (entry->key == key) {
            // Found it - remove from chain
            if (prev != NULL) {
                prev->next = entry->next;
            } else {
                map->buckets[hash] = entry->next;
            }
            free(entry);
            return;
        }
        prev = entry;
        entry = entry->next;
    }
}

/**
 * Clear entire order map
 */
static void order_map_clear(order_map_t* map) {
    for (int i = 0; i < ORDER_MAP_SIZE; i++) {
        order_map_entry_t* entry = map->buckets[i];
        while (entry != NULL) {
            order_map_entry_t* next = entry->next;
            free(entry);
            entry = next;
        }
        map->buckets[i] = NULL;
    }
}

/* ============================================================================
 * Helper Functions - Price Level Management (Binary Search)
 * ============================================================================ */

/**
 * Find price level in sorted array (binary search)
 * Returns index if found, -1 if not found
 */
static int find_price_level(const price_level_t* levels, int num_levels, uint32_t price, bool descending) {
    int left = 0;
    int right = num_levels - 1;
    
    while (left <= right) {
        int mid = left + (right - left) / 2;
        
        if (levels[mid].price == price) {
            return mid;
        }
        
        if (descending) {
            // Descending order (bids)
            if (levels[mid].price > price) {
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        } else {
            // Ascending order (asks)
            if (levels[mid].price < price) {
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        }
    }
    
    return -1;  // Not found
}

/**
 * Insert price level into sorted array
 * Returns index where inserted
 */
static int insert_price_level(price_level_t* levels, int* num_levels, uint32_t price, bool descending) {
    // Find insertion point
    int insert_pos = *num_levels;
    
    for (int i = 0; i < *num_levels; i++) {
        bool should_insert_here;
        if (descending) {
            should_insert_here = (price > levels[i].price);
        } else {
            should_insert_here = (price < levels[i].price);
        }
        
        if (should_insert_here) {
            insert_pos = i;
            break;
        }
    }
    
    // Shift elements to make room
    if (insert_pos < *num_levels) {
        memmove(&levels[insert_pos + 1], &levels[insert_pos], 
                (*num_levels - insert_pos) * sizeof(price_level_t));
    }
    
    // Initialize new level
    levels[insert_pos].price = price;
    levels[insert_pos].total_quantity = 0;
    levels[insert_pos].orders_head = NULL;
    levels[insert_pos].orders_tail = NULL;
    levels[insert_pos].active = true;
    
    (*num_levels)++;
    
    return insert_pos;
}

/**
 * Remove price level from sorted array
 */
static void remove_price_level(price_level_t* levels, int* num_levels, int index) {
    if (index < 0 || index >= *num_levels) {
        return;
    }
    
    // Free all orders at this level
    list_free_all(levels[index].orders_head);
    
    // Shift elements down
    if (index < *num_levels - 1) {
        memmove(&levels[index], &levels[index + 1], 
                (*num_levels - index - 1) * sizeof(price_level_t));
    }
    
    (*num_levels)--;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/**
 * Initialize order book for a symbol
 */
void order_book_init(order_book_t* book, const char* symbol) {
    strncpy(book->symbol, symbol, MAX_SYMBOL_LENGTH - 1);
    book->symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
    
    // Initialize price levels
    memset(book->bids, 0, sizeof(book->bids));
    memset(book->asks, 0, sizeof(book->asks));
    book->num_bid_levels = 0;
    book->num_ask_levels = 0;
    
    // Initialize order map
    memset(&book->order_map, 0, sizeof(order_map_t));
    
    // Initialize TOB tracking
    book->prev_best_bid_price = 0;
    book->prev_best_bid_qty = 0;
    book->prev_best_ask_price = 0;
    book->prev_best_ask_qty = 0;

    book->bid_side_ever_active = false;
    book->ask_side_ever_active = false;
}

/**
 * Destroy order book and free all memory
 */
void order_book_destroy(order_book_t* book) {
    // Free all bid levels
    for (int i = 0; i < book->num_bid_levels; i++) {
        list_free_all(book->bids[i].orders_head);
    }
    
    // Free all ask levels
    for (int i = 0; i < book->num_ask_levels; i++) {
        list_free_all(book->asks[i].orders_head);
    }
    
    // Clear order map
    order_map_clear(&book->order_map);
}

/**
 * Add order to price level
 */
static void add_order_to_level(order_book_t* book, price_level_t* level, order_t* order) {
    // Add to list
    list_append(&level->orders_head, &level->orders_tail, order);
    
    // Update total quantity
    level->total_quantity += order->remaining_qty;
    
    // Add to order map for cancellation
    uint64_t key = make_order_key(order->user_id, order->user_order_id);
    order_location_t location;
    location.side = order->side;
    location.price = order->price;
    location.order_ptr = order;
    order_map_insert(&book->order_map, key, &location);
}

/**
 * Match an incoming order against the book
 */
static void match_order(order_book_t* book, order_t* order, output_buffer_t* output) {
    if (order->side == SIDE_BUY) {
        // Buy order: match against asks (ascending price)
        while (order->remaining_qty > 0 && book->num_ask_levels > 0) {
            price_level_t* best_ask_level = &book->asks[0];  // Lowest price (best ask)
            
            // Check if we can match
            bool can_match = (order->type == ORDER_TYPE_MARKET) || 
                            (order->price >= best_ask_level->price);
            
            if (!can_match) {
                break;
            }
            
            // Match with orders at this price level (FIFO)
            order_t* passive_order = best_ask_level->orders_head;
            
            while (order->remaining_qty > 0 && passive_order != NULL) {
                order_t* next_order = passive_order->next;
                
                // Calculate trade quantity
                uint32_t trade_qty = (order->remaining_qty < passive_order->remaining_qty) ?
                                     order->remaining_qty : passive_order->remaining_qty;
                
                // Generate trade message
                output_msg_t trade_msg = make_trade_msg(
                    book->symbol,
                    order->user_id, 
                    order->user_order_id,
                    passive_order->user_id, 
                    passive_order->user_order_id,
                    best_ask_level->price,  // Trade at passive order price
                    trade_qty
                );
                output_buffer_add(output, &trade_msg);
                
                // Update quantities
                order_fill(order, trade_qty);
                order_fill(passive_order, trade_qty);
                best_ask_level->total_quantity -= trade_qty;
                
                // Remove passive order if fully filled
                if (order_is_filled(passive_order)) {
                    uint64_t key = make_order_key(passive_order->user_id, passive_order->user_order_id);
                    order_map_remove(&book->order_map, key);
                    list_remove(&best_ask_level->orders_head, &best_ask_level->orders_tail, passive_order);
                    free(passive_order);
                }
                
                passive_order = next_order;
            }
            
            // Remove price level if empty
            if (best_ask_level->orders_head == NULL) {
                remove_price_level(book->asks, &book->num_ask_levels, 0);
            }
        }
    } else {
        // Sell order: match against bids (descending price)
        while (order->remaining_qty > 0 && book->num_bid_levels > 0) {
            price_level_t* best_bid_level = &book->bids[0];  // Highest price (best bid)
            
            // Check if we can match
            bool can_match = (order->type == ORDER_TYPE_MARKET) || 
                            (order->price <= best_bid_level->price);
            
            if (!can_match) {
                break;
            }
            
            // Match with orders at this price level (FIFO)
            order_t* passive_order = best_bid_level->orders_head;
            
            while (order->remaining_qty > 0 && passive_order != NULL) {
                order_t* next_order = passive_order->next;
                
                // Calculate trade quantity
                uint32_t trade_qty = (order->remaining_qty < passive_order->remaining_qty) ?
                                     order->remaining_qty : passive_order->remaining_qty;
                
                // Generate trade message
                output_msg_t trade_msg = make_trade_msg(
                    book->symbol,
                    passive_order->user_id, 
                    passive_order->user_order_id,
                    order->user_id, 
                    order->user_order_id,
                    best_bid_level->price,  // Trade at passive order price
                    trade_qty
                );
                output_buffer_add(output, &trade_msg);
                
                // Update quantities
                order_fill(order, trade_qty);
                order_fill(passive_order, trade_qty);
                best_bid_level->total_quantity -= trade_qty;
                
                // Remove passive order if fully filled
                if (order_is_filled(passive_order)) {
                    uint64_t key = make_order_key(passive_order->user_id, passive_order->user_order_id);
                    order_map_remove(&book->order_map, key);
                    list_remove(&best_bid_level->orders_head, &best_bid_level->orders_tail, passive_order);
                    free(passive_order);
                }
                
                passive_order = next_order;
            }
            
            // Remove price level if empty
            if (best_bid_level->orders_head == NULL) {
                remove_price_level(book->bids, &book->num_bid_levels, 0);
            }
        }
    }
}

/**
 * Add order to book (for limit orders that don't fully match)
 */
static void add_to_book(order_book_t* book, order_t* order) {
    if (order->side == SIDE_BUY) {
        // Find or create price level
        int idx = find_price_level(book->bids, book->num_bid_levels, order->price, true);
        
        if (idx == -1) {
            // Price level doesn't exist - create it
            idx = insert_price_level(book->bids, &book->num_bid_levels, order->price, true);
        }
        
        add_order_to_level(book, &book->bids[idx], order);
    } else {
        // Find or create price level
        int idx = find_price_level(book->asks, book->num_ask_levels, order->price, false);
        
        if (idx == -1) {
            // Price level doesn't exist - create it
            idx = insert_price_level(book->asks, &book->num_ask_levels, order->price, false);
        }
        
        add_order_to_level(book, &book->asks[idx], order);
    }
}

/**
 * Check for top-of-book changes and generate TOB messages
 */

static void check_tob_changes(order_book_t* book, output_buffer_t* output) {
    uint32_t current_best_bid_price = order_book_get_best_bid_price(book);
    uint32_t current_best_bid_qty = order_book_get_best_bid_quantity(book);
    uint32_t current_best_ask_price = order_book_get_best_ask_price(book);
    uint32_t current_best_ask_qty = order_book_get_best_ask_quantity(book);
    
    /* Track if sides ever become active */
    if (current_best_bid_price > 0) {
        book->bid_side_ever_active = true;
    }
    if (current_best_ask_price > 0) {
        book->ask_side_ever_active = true;
    }
    
    /* Check bid side changes */
    if (current_best_bid_price != book->prev_best_bid_price || 
        current_best_bid_qty != book->prev_best_bid_qty) {
        
        if (current_best_bid_price == 0 && book->bid_side_ever_active) {
            /* Bid side eliminated */
            output_msg_t msg = make_top_of_book_eliminated_msg(book->symbol, SIDE_BUY);
            output_buffer_add(output, &msg);
        } else if (current_best_bid_price > 0) {
            output_msg_t msg = make_top_of_book_msg(book->symbol, SIDE_BUY, current_best_bid_price, current_best_bid_qty);
            output_buffer_add(output, &msg);
        }
        
        book->prev_best_bid_price = current_best_bid_price;
        book->prev_best_bid_qty = current_best_bid_qty;
    }
    
    /* Check ask side changes */
    if (current_best_ask_price != book->prev_best_ask_price || 
        current_best_ask_qty != book->prev_best_ask_qty) {
        
        if (current_best_ask_price == 0 && book->ask_side_ever_active) {
            /* Ask side eliminated */
            output_msg_t msg = make_top_of_book_eliminated_msg(book->symbol, SIDE_SELL);
            output_buffer_add(output, &msg);
        } else if (current_best_ask_price > 0) {
            output_msg_t msg = make_top_of_book_msg(book->symbol, SIDE_SELL, current_best_ask_price, current_best_ask_qty);
            output_buffer_add(output, &msg);
        }
        
        book->prev_best_ask_price = current_best_ask_price;
        book->prev_best_ask_qty = current_best_ask_qty;
    }
}

/**
 * Process new order
 */
void order_book_add_order(order_book_t* book, const new_order_msg_t* msg, output_buffer_t* output) {
    // Create order with timestamp
    uint64_t timestamp = order_get_current_timestamp();
    order_t* order = (order_t*)malloc(sizeof(order_t));
    order_init(order, msg, timestamp);
    
    // Send acknowledgement
    output_msg_t ack = make_ack_msg(book->symbol, order->user_id, order->user_order_id);
    output_buffer_add(output, &ack);
    
    // Try to match the order
    match_order(book, order, output);
    
    // If order has remaining quantity and is a limit order, add to book
    if (order->remaining_qty > 0 && order->type == ORDER_TYPE_LIMIT) {
        add_to_book(book, order);
    } else {
        // Order fully filled or market order - free it
        free(order);
    }
    
    // Check for top-of-book changes
    check_tob_changes(book, output);
}

/**
 * Cancel order
 */
void order_book_cancel_order(order_book_t* book, uint32_t user_id, uint32_t user_order_id, 
                              output_buffer_t* output) {
    uint64_t key = make_order_key(user_id, user_order_id);
    order_map_entry_t* entry = order_map_find(&book->order_map, key);
    
    if (entry == NULL) {
        // Order not found - still send cancel ack
        output_msg_t msg = make_cancel_ack_msg(book->symbol, user_id, user_order_id);
        output_buffer_add(output, &msg);
        return;
    }
    
    // Get order location
    order_location_t* loc = &entry->location;
    order_t* order = loc->order_ptr;
    
    // Find price level
    price_level_t* levels;
    int* num_levels;
    bool descending;
    
    if (loc->side == SIDE_BUY) {
        levels = book->bids;
        num_levels = &book->num_bid_levels;
        descending = true;
    } else {
        levels = book->asks;
        num_levels = &book->num_ask_levels;
        descending = false;
    }
    
    int idx = find_price_level(levels, *num_levels, loc->price, descending);
    
    if (idx != -1) {
        price_level_t* level = &levels[idx];
        
        // Update total quantity
        level->total_quantity -= order->remaining_qty;
        
        // Remove from list
        list_remove(&level->orders_head, &level->orders_tail, order);
        
        // Free order
        free(order);
        
        // Remove price level if empty
        if (level->orders_head == NULL) {
            remove_price_level(levels, num_levels, idx);
        }
    }
    
    // Remove from order map
    order_map_remove(&book->order_map, key);
    
    // Send cancel acknowledgement
    output_msg_t msg = make_cancel_ack_msg(book->symbol, user_id, user_order_id);
    output_buffer_add(output, &msg);
    
    // Check for top-of-book changes
    check_tob_changes(book, output);
}

/**
 * Flush/clear the entire order book
 */
void order_book_flush(order_book_t* book) {
    // Free all bid levels
    for (int i = 0; i < book->num_bid_levels; i++) {
        list_free_all(book->bids[i].orders_head);
        book->bids[i].orders_head = NULL;
        book->bids[i].orders_tail = NULL;
        book->bids[i].total_quantity = 0;
    }
    book->num_bid_levels = 0;
    
    // Free all ask levels
    for (int i = 0; i < book->num_ask_levels; i++) {
        list_free_all(book->asks[i].orders_head);
        book->asks[i].orders_head = NULL;
        book->asks[i].orders_tail = NULL;
        book->asks[i].total_quantity = 0;
    }
    book->num_ask_levels = 0;
    
    // Clear order map
    order_map_clear(&book->order_map);
    
    // Reset TOB tracking
    book->prev_best_bid_price = 0;
    book->prev_best_bid_qty = 0;
    book->prev_best_ask_price = 0;
    book->prev_best_ask_qty = 0;
}

/**
 * Get best bid price (0 if none)
 */
uint32_t order_book_get_best_bid_price(const order_book_t* book) {
    return (book->num_bid_levels > 0) ? book->bids[0].price : 0;
}

/**
 * Get best ask price (0 if none)
 */
uint32_t order_book_get_best_ask_price(const order_book_t* book) {
    return (book->num_ask_levels > 0) ? book->asks[0].price : 0;
}

/**
 * Get total quantity at best bid
 */
uint32_t order_book_get_best_bid_quantity(const order_book_t* book) {
    return (book->num_bid_levels > 0) ? book->bids[0].total_quantity : 0;
}

/**
 * Get total quantity at best ask
 */
uint32_t order_book_get_best_ask_quantity(const order_book_t* book) {
    return (book->num_ask_levels > 0) ? book->asks[0].total_quantity : 0;
}

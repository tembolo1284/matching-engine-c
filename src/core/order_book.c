#include "core/order_book.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Memory Pool Implementation (Rule 3 - No malloc after init)
 * ============================================================================ */

/**
 * Initialize order pool - called once at startup
 */
static void order_pool_init(order_pool_t* pool) {
    pool->free_count = MAX_ORDERS_IN_POOL;
    
    // Initialize free list (all indices available)
    for (int i = 0; i < MAX_ORDERS_IN_POOL; i++) {
        pool->free_list[i] = i;
    }
    
    // Zero out statistics
    pool->total_allocations = 0;
    pool->peak_usage = 0;
    pool->allocation_failures = 0;
}

/**
 * Initialize hash entry pool - called once at startup
 */
static void hash_entry_pool_init(hash_entry_pool_t* pool) {
    pool->free_count = MAX_HASH_ENTRIES_IN_POOL;
    
    for (int i = 0; i < MAX_HASH_ENTRIES_IN_POOL; i++) {
        pool->free_list[i] = i;
    }
    
    pool->total_allocations = 0;
    pool->peak_usage = 0;
    pool->allocation_failures = 0;
}

/**
 * Initialize all memory pools (PUBLIC API)
 */
void memory_pools_init(memory_pools_t* pools) {
    order_pool_init(&pools->order_pool);
    hash_entry_pool_init(&pools->hash_entry_pool);
}

/**
 * Get memory pool statistics (PUBLIC API)
 */
void memory_pools_get_stats(const memory_pools_t* pools, memory_pool_stats_t* stats) {
    stats->order_allocations = pools->order_pool.total_allocations;
    stats->order_peak_usage = pools->order_pool.peak_usage;
    stats->order_failures = pools->order_pool.allocation_failures;
    stats->hash_allocations = pools->hash_entry_pool.total_allocations;
    stats->hash_peak_usage = pools->hash_entry_pool.peak_usage;
    stats->hash_failures = pools->hash_entry_pool.allocation_failures;
    stats->total_memory_bytes = 
        (MAX_ORDERS_IN_POOL * sizeof(order_t)) +
        (MAX_HASH_ENTRIES_IN_POOL * sizeof(order_map_entry_t));
}

/**
 * Allocate order from pool (replaces malloc)
 * Performance: O(1), ~5-10 CPU cycles
 */
static inline order_t* order_pool_alloc(order_pool_t* pool) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (pool == NULL) {
        fprintf(stderr, "ERROR: NULL pool in order_pool_alloc\n");
        return NULL;
    }
    #endif
    
    if (pool->free_count == 0) {
        // Pool exhausted - critical error
        pool->allocation_failures++;
        #ifdef DEBUG
        fprintf(stderr, "ERROR: Order pool exhausted! Consider increasing MAX_ORDERS_IN_POOL\n");
        #endif
        return NULL;
    }
    
    // Pop from free list
    pool->free_count--;
    uint32_t index = pool->free_list[pool->free_count];
    
    // Update statistics
    pool->total_allocations++;
    uint32_t current_usage = MAX_ORDERS_IN_POOL - pool->free_count;
    if (current_usage > pool->peak_usage) {
        pool->peak_usage = current_usage;
    }
    
    // Return pointer to pre-allocated order
    return &pool->orders[index];
}

/**
 * Free order back to pool (replaces free)
 * Performance: O(1), ~5-10 CPU cycles
 */
static inline void order_pool_free(order_pool_t* pool, order_t* order) {
    // Rule 7: Validate parameters
    #ifdef DEBUG
    if (pool == NULL || order == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in order_pool_free\n");
        return;
    }
    
    // Validate that order is actually from our pool
    if (order < pool->orders || order >= pool->orders + MAX_ORDERS_IN_POOL) {
        fprintf(stderr, "ERROR: Attempting to free order not from pool!\n");
        return;
    }
    
    // Rule 5: Assertion for invariant
    if (pool->free_count >= MAX_ORDERS_IN_POOL) {
        fprintf(stderr, "ERROR: Freeing to already-full pool (double-free?)\n");
        return;
    }
    #endif
    
    // Calculate index from pointer arithmetic
    uint32_t index = (uint32_t)(order - pool->orders);
    
    // Push to free list
    pool->free_list[pool->free_count] = index;
    pool->free_count++;
}

/**
 * Allocate hash entry from pool (replaces malloc)
 */
static inline order_map_entry_t* hash_entry_pool_alloc(hash_entry_pool_t* pool) {
    #ifdef DEBUG
    if (pool == NULL) {
        fprintf(stderr, "ERROR: NULL pool in hash_entry_pool_alloc\n");
        return NULL;
    }
    #endif
    
    if (pool->free_count == 0) {
        pool->allocation_failures++;
        #ifdef DEBUG
        fprintf(stderr, "ERROR: Hash entry pool exhausted!\n");
        #endif
        return NULL;
    }
    
    pool->free_count--;
    uint32_t index = pool->free_list[pool->free_count];
    
    pool->total_allocations++;
    uint32_t current_usage = MAX_HASH_ENTRIES_IN_POOL - pool->free_count;
    if (current_usage > pool->peak_usage) {
        pool->peak_usage = current_usage;
    }
    
    return &pool->entries[index];
}

/**
 * Free hash entry back to pool (replaces free)
 */
static inline void hash_entry_pool_free(hash_entry_pool_t* pool, order_map_entry_t* entry) {
    #ifdef DEBUG
    if (pool == NULL || entry == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in hash_entry_pool_free\n");
        return;
    }
    
    if (entry < pool->entries || entry >= pool->entries + MAX_HASH_ENTRIES_IN_POOL) {
        fprintf(stderr, "ERROR: Attempting to free entry not from pool!\n");
        return;
    }
    
    if (pool->free_count >= MAX_HASH_ENTRIES_IN_POOL) {
        fprintf(stderr, "ERROR: Double-free detected in hash entry pool!\n");
        return;
    }
    #endif
    
    uint32_t index = (uint32_t)(entry - pool->entries);
    pool->free_list[pool->free_count] = index;
    pool->free_count++;
}

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
 * Free all orders in a list (now uses pool)
 */
static void list_free_all(order_t* head, order_pool_t* pool) {
    order_t* current = head;
    
    // Rule 2: Bounded loop
    int count = 0;
    const int MAX_ORDERS = MAX_ORDERS_IN_POOL;
    
    while (current != NULL && count < MAX_ORDERS) {
        order_t* next = current->next;
        order_pool_free(pool, current);
        current = next;
        count++;
    }
    
    #ifdef DEBUG
    if (count >= MAX_ORDERS) {
        fprintf(stderr, "WARNING: list_free_all hit iteration limit\n");
    }
    #endif
}

/* ============================================================================
 * Helper Functions - Hash Table Operations
 * ============================================================================ */

/**
 * Hash function for order key
 */
static uint32_t hash_order_key(uint64_t key) {
    return (uint32_t)(key % ORDER_MAP_SIZE);
}

/**
 * Insert into order map (now uses pool)
 */
static void order_map_insert(order_map_t* map, uint64_t key, 
                              const order_location_t* location,
                              hash_entry_pool_t* pool) {
    uint32_t hash = hash_order_key(key);

    // Allocate from pool instead of malloc
    order_map_entry_t* entry = hash_entry_pool_alloc(pool);
    
    // Rule 7: Check allocation success
    if (entry == NULL) {
        #ifdef DEBUG
        fprintf(stderr, "ERROR: Failed to allocate hash entry\n");
        #endif
        return;
    }
    
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

    // Rule 2: Bounded loop
    int count = 0;
    while (entry != NULL && count < MAX_HASH_CHAIN_LENGTH) {
        if (entry->key == key) {
            return entry;
        }
        entry = entry->next;
        count++;
    }

    return NULL;
}

/**
 * Remove from order map (now uses pool)
 */
static void order_map_remove(order_map_t* map, uint64_t key, hash_entry_pool_t* pool) {
    uint32_t hash = hash_order_key(key);
    order_map_entry_t* entry = map->buckets[hash];
    order_map_entry_t* prev = NULL;

    // Rule 2: Bounded loop
    int count = 0;
    while (entry != NULL && count < MAX_HASH_CHAIN_LENGTH) {
        if (entry->key == key) {
            // Found it - remove from chain
            if (prev != NULL) {
                prev->next = entry->next;
            } else {
                map->buckets[hash] = entry->next;
            }
            hash_entry_pool_free(pool, entry);
            return;
        }
        prev = entry;
        entry = entry->next;
        count++;
    }
}

/**
 * Clear entire order map (now uses pool)
 */
static void order_map_clear(order_map_t* map, hash_entry_pool_t* pool) {
    // Rule 2: Fixed bounds
    for (int i = 0; i < ORDER_MAP_SIZE; i++) {
        order_map_entry_t* entry = map->buckets[i];
        
        int count = 0;
        while (entry != NULL && count < MAX_HASH_CHAIN_LENGTH) {
            order_map_entry_t* next = entry->next;
            hash_entry_pool_free(pool, entry);
            entry = next;
            count++;
        }
        map->buckets[i] = NULL;
    }
}

/* ============================================================================
 * Helper Functions - Price Level Management
 * ============================================================================ */

/**
 * Find price level in sorted array (binary search)
 * Returns index if found, -1 if not found
 */
static int find_price_level(const price_level_t* levels, int num_levels, 
                             uint32_t price, bool descending) {
    int left = 0;
    int right = num_levels - 1;

    // Rule 2: Binary search is naturally bounded by log(num_levels)
    while (left <= right) {
        int mid = left + (right - left) / 2;

        if (levels[mid].price == price) {
            return mid;
        }

        if (descending) {
            if (levels[mid].price > price) {
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        } else {
            if (levels[mid].price < price) {
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        }
    }

    return -1;
}

/**
 * Insert price level into sorted array
 */
static int insert_price_level(price_level_t* levels, int* num_levels, 
                               uint32_t price, bool descending) {
    // Rule 2: Bounded by MAX_PRICE_LEVELS
    int insert_pos = *num_levels;

    for (int i = 0; i < *num_levels && i < MAX_PRICE_LEVELS; i++) {
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
static void remove_price_level(price_level_t* levels, int* num_levels, 
                                int index, order_pool_t* pool) {
    if (index < 0 || index >= *num_levels) {
        return;
    }

    // Free all orders at this level
    list_free_all(levels[index].orders_head, pool);

    // Shift elements down
    if (index < *num_levels - 1) {
        memmove(&levels[index], &levels[index + 1],
                (*num_levels - index - 1) * sizeof(price_level_t));
    }

    (*num_levels)--;
}

/* ============================================================================
 * Matching Engine - Trade Execution (Rule 4: Small functions)
 * ============================================================================ */

/**
 * Execute a single trade between aggressor and passive order
 * Returns true if passive order was fully filled and removed
 */
static inline bool execute_trade(
    order_book_t* book,
    order_t* aggressor,
    order_t* passive,
    price_level_t* level,
    uint32_t trade_price,
    output_buffer_t* output)
{
    // Calculate trade quantity
    uint32_t trade_qty = (aggressor->remaining_qty < passive->remaining_qty) ?
                         aggressor->remaining_qty : passive->remaining_qty;

    // Determine buy/sell sides based on aggressor
    uint32_t buy_user_id, buy_order_id, buy_client_id;
    uint32_t sell_user_id, sell_order_id, sell_client_id;
    
    if (aggressor->side == SIDE_BUY) {
        buy_user_id = aggressor->user_id;
        buy_order_id = aggressor->user_order_id;
        buy_client_id = aggressor->client_id;
        sell_user_id = passive->user_id;
        sell_order_id = passive->user_order_id;
        sell_client_id = passive->client_id;
    } else {
        buy_user_id = passive->user_id;
        buy_order_id = passive->user_order_id;
        buy_client_id = passive->client_id;
        sell_user_id = aggressor->user_id;
        sell_order_id = aggressor->user_order_id;
        sell_client_id = aggressor->client_id;
    }

    // Generate trade message
    output_msg_t trade_msg = make_trade_msg(
        book->symbol,
        buy_user_id,
        buy_order_id,
        sell_user_id,
        sell_order_id,
        trade_price,
        trade_qty
    );
    
    trade_msg.data.trade.buy_client_id = buy_client_id;
    trade_msg.data.trade.sell_client_id = sell_client_id;
    
    output_buffer_add(output, &trade_msg);

    // Update quantities
    order_fill(aggressor, trade_qty);
    order_fill(passive, trade_qty);
    level->total_quantity -= trade_qty;

    // Check if passive order is fully filled
    bool passive_filled = order_is_filled(passive);
    
    if (passive_filled) {
        uint64_t key = make_order_key(passive->user_id, passive->user_order_id);
        order_map_remove(&book->order_map, key, &book->pools->hash_entry_pool);
        list_remove(&level->orders_head, &level->orders_tail, passive);
        order_pool_free(&book->pools->order_pool, passive);
    }

    return passive_filled;
}

/* ============================================================================
 * Matching Engine - Buy Side Matcher (Rule 4: ≤60 lines)
 * ============================================================================ */

/**
 * Match buy order against asks (ascending price order)
 * Buy orders match with lowest ask prices first
 */
static inline void match_buy_order(
    order_book_t* book,
    order_t* order,
    output_buffer_t* output)
{
    // Rule 2: Fixed upper bound on iterations
    int iteration_count = 0;

    // Match against asks (best ask = lowest price = index 0)
    while (order->remaining_qty > 0 && 
           book->num_ask_levels > 0 &&
           iteration_count < MAX_MATCH_ITERATIONS) {
        
        iteration_count++;
        
        price_level_t* best_ask_level = &book->asks[0];

        // Check if we can match at this price
        bool can_match = (order->type == ORDER_TYPE_MARKET) ||
                        (order->price >= best_ask_level->price);

        if (!can_match) {
            break;
        }

        // Match with orders at this price level (FIFO)
        order_t* passive_order = best_ask_level->orders_head;
        
        // Rule 2: Bound the inner loop
        int inner_iteration = 0;
        
        while (order->remaining_qty > 0 && 
               passive_order != NULL &&
               inner_iteration < MAX_ORDERS_AT_PRICE_LEVEL) {
            
            inner_iteration++;
            order_t* next_order = passive_order->next;

            // Execute the trade
            execute_trade(book, order, passive_order, best_ask_level,
                         best_ask_level->price, output);

            passive_order = next_order;
        }

        // Remove price level if empty
        if (best_ask_level->orders_head == NULL) {
            remove_price_level(book->asks, &book->num_ask_levels, 0, 
                             &book->pools->order_pool);
        }
    }

    #ifdef DEBUG
    // Rule 5: Assertion to catch runaway matching
    if (iteration_count >= MAX_MATCH_ITERATIONS) {
        fprintf(stderr, "WARNING: Buy order matching hit iteration limit\n");
    }
    #endif
}

/* ============================================================================
 * Matching Engine - Sell Side Matcher (Rule 4: ≤60 lines)
 * ============================================================================ */

/**
 * Match sell order against bids (descending price order)
 * Sell orders match with highest bid prices first
 */
static inline void match_sell_order(
    order_book_t* book,
    order_t* order,
    output_buffer_t* output)
{
    // Rule 2: Fixed upper bound on iterations
    int iteration_count = 0;

    // Match against bids (best bid = highest price = index 0)
    while (order->remaining_qty > 0 && 
           book->num_bid_levels > 0 &&
           iteration_count < MAX_MATCH_ITERATIONS) {
        
        iteration_count++;
        
        price_level_t* best_bid_level = &book->bids[0];

        // Check if we can match at this price
        bool can_match = (order->type == ORDER_TYPE_MARKET) ||
                        (order->price <= best_bid_level->price);

        if (!can_match) {
            break;
        }

        // Match with orders at this price level (FIFO)
        order_t* passive_order = best_bid_level->orders_head;
        
        // Rule 2: Bound the inner loop
        int inner_iteration = 0;
        
        while (order->remaining_qty > 0 && 
               passive_order != NULL &&
               inner_iteration < MAX_ORDERS_AT_PRICE_LEVEL) {
            
            inner_iteration++;
            order_t* next_order = passive_order->next;

            // Execute the trade
            execute_trade(book, order, passive_order, best_bid_level,
                         best_bid_level->price, output);

            passive_order = next_order;
        }

        // Remove price level if empty
        if (best_bid_level->orders_head == NULL) {
            remove_price_level(book->bids, &book->num_bid_levels, 0,
                             &book->pools->order_pool);
        }
    }

    #ifdef DEBUG
    // Rule 5: Assertion to catch runaway matching
    if (iteration_count >= MAX_MATCH_ITERATIONS) {
        fprintf(stderr, "WARNING: Sell order matching hit iteration limit\n");
    }
    #endif
}

/* ============================================================================
 * Matching Engine - Dispatcher (Rule 4: Simple function)
 * ============================================================================ */

/**
 * Match an incoming order against the book
 * Dispatches to side-specific matcher for better performance
 */
static inline void match_order(
    order_book_t* book,
    order_t* order,
    output_buffer_t* output)
{
    if (order->side == SIDE_BUY) {
        match_buy_order(book, order, output);
    } else {
        match_sell_order(book, order, output);
    }
}

/* ============================================================================
 * Order Book Operations
 * ============================================================================ */

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
    order_map_insert(&book->order_map, key, &location, &book->pools->hash_entry_pool);
}

/**
 * Add order to book (for limit orders that don't fully match)
 */
static void add_to_book(order_book_t* book, order_t* order) {
    if (order->side == SIDE_BUY) {
        int idx = find_price_level(book->bids, book->num_bid_levels, order->price, true);

        if (idx == -1) {
            idx = insert_price_level(book->bids, &book->num_bid_levels, order->price, true);
        }

        add_order_to_level(book, &book->bids[idx], order);
    } else {
        int idx = find_price_level(book->asks, book->num_ask_levels, order->price, false);

        if (idx == -1) {
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
            output_msg_t msg = make_top_of_book_eliminated_msg(book->symbol, SIDE_BUY);
            output_buffer_add(output, &msg);
        } else if (current_best_bid_price > 0) {
            output_msg_t msg = make_top_of_book_msg(book->symbol, SIDE_BUY, 
                                                     current_best_bid_price, current_best_bid_qty);
            output_buffer_add(output, &msg);
        }

        book->prev_best_bid_price = current_best_bid_price;
        book->prev_best_bid_qty = current_best_bid_qty;
    }

    /* Check ask side changes */
    if (current_best_ask_price != book->prev_best_ask_price ||
        current_best_ask_qty != book->prev_best_ask_qty) {

        if (current_best_ask_price == 0 && book->ask_side_ever_active) {
            output_msg_t msg = make_top_of_book_eliminated_msg(book->symbol, SIDE_SELL);
            output_buffer_add(output, &msg);
        } else if (current_best_ask_price > 0) {
            output_msg_t msg = make_top_of_book_msg(book->symbol, SIDE_SELL,
                                                     current_best_ask_price, current_best_ask_qty);
            output_buffer_add(output, &msg);
        }

        book->prev_best_ask_price = current_best_ask_price;
        book->prev_best_ask_qty = current_best_ask_qty;
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/**
 * Initialize order book for a symbol
 */
void order_book_init(order_book_t* book, const char* symbol, memory_pools_t* pools) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (book == NULL || symbol == NULL || pools == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in order_book_init\n");
        return;
    }
    #endif

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

    // Store reference to memory pools
    book->pools = pools;
}

/**
 * Destroy order book and return all memory to pools
 */
void order_book_destroy(order_book_t* book) {
    #ifdef DEBUG
    if (book == NULL) {
        fprintf(stderr, "ERROR: NULL book in order_book_destroy\n");
        return;
    }
    #endif

    // Free all bid levels
    for (int i = 0; i < book->num_bid_levels; i++) {
        list_free_all(book->bids[i].orders_head, &book->pools->order_pool);
    }

    // Free all ask levels
    for (int i = 0; i < book->num_ask_levels; i++) {
        list_free_all(book->asks[i].orders_head, &book->pools->order_pool);
    }

    // Clear order map
    order_map_clear(&book->order_map, &book->pools->hash_entry_pool);
}

/**
 * Process new order (now uses pool allocation)
 */
void order_book_add_order(order_book_t* book, 
                          const new_order_msg_t* msg,
                          uint32_t client_id,
                          output_buffer_t* output) {
    // Rule 7: Parameter validation
    #ifdef DEBUG
    if (book == NULL || msg == NULL || output == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in order_book_add_order\n");
        return;
    }
    #endif

    // Allocate from pool instead of malloc
    order_t* order = order_pool_alloc(&book->pools->order_pool);
    
    // Rule 7: Check allocation success
    if (order == NULL) {
        // Pool exhausted - send reject message
        output_msg_t reject = make_reject_msg(book->symbol, 
                                              msg->user_id, 
                                              msg->user_order_id,
                                              "Order pool exhausted");
        output_buffer_add(output, &reject);
        return;
    }
    
    // Initialize order
    uint64_t timestamp = order_get_current_timestamp();
    order_init(order, msg, timestamp);
    order->client_id = client_id;

    // Send acknowledgement
    output_msg_t ack = make_ack_msg(book->symbol, order->user_id, order->user_order_id);
    output_buffer_add(output, &ack);

    // Try to match the order
    match_order(book, order, output);

    // If order has remaining quantity and is a limit order, add to book
    if (order->remaining_qty > 0 && order->type == ORDER_TYPE_LIMIT) {
        add_to_book(book, order);
    } else {
        // Order fully filled or market order - return to pool
        order_pool_free(&book->pools->order_pool, order);
    }

    // Check for top-of-book changes
    check_tob_changes(book, output);
}

/**
 * Cancel order
 */
void order_book_cancel_order(order_book_t* book, uint32_t user_id, uint32_t user_order_id,
                              output_buffer_t* output) {
    #ifdef DEBUG
    if (book == NULL || output == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in order_book_cancel_order\n");
        return;
    }
    #endif

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

        // Return to pool
        order_pool_free(&book->pools->order_pool, order);

        // Remove price level if empty
        if (level->orders_head == NULL) {
            remove_price_level(levels, num_levels, idx, &book->pools->order_pool);
        }
    }

    // Remove from order map
    order_map_remove(&book->order_map, key, &book->pools->hash_entry_pool);

    // Send cancel acknowledgement
    output_msg_t msg = make_cancel_ack_msg(book->symbol, user_id, user_order_id);
    output_buffer_add(output, &msg);

    // Check for top-of-book changes
    check_tob_changes(book, output);
}

/**
 * Cancel all orders for a specific client (TCP mode)
 */
size_t order_book_cancel_client_orders(order_book_t* book,
                                       uint32_t client_id,
                                       output_buffer_t* output) {
    size_t cancelled_count = 0;
    
    // Rule 2: Bounded loops
    for (int i = 0; i < book->num_bid_levels && i < MAX_PRICE_LEVELS; i++) {
        order_t* order = book->bids[i].orders_head;
        int order_count = 0;
        
        while (order != NULL && order_count < MAX_ORDERS_AT_PRICE_LEVEL) {
            order_t* next_order = order->next;
            
            if (order->client_id == client_id) {
                order_book_cancel_order(book, order->user_id, order->user_order_id, output);
                cancelled_count++;
            }
            
            order = next_order;
            order_count++;
        }
    }
    
    for (int i = 0; i < book->num_ask_levels && i < MAX_PRICE_LEVELS; i++) {
        order_t* order = book->asks[i].orders_head;
        int order_count = 0;
        
        while (order != NULL && order_count < MAX_ORDERS_AT_PRICE_LEVEL) {
            order_t* next_order = order->next;
            
            if (order->client_id == client_id) {
                order_book_cancel_order(book, order->user_id, order->user_order_id, output);
                cancelled_count++;
            }
            
            order = next_order;
            order_count++;
        }
    }
    
    return cancelled_count;
}

/**
 * Flush/clear the entire order book
 */
void order_book_flush(order_book_t* book, output_buffer_t* output) {
    #ifdef DEBUG
    if (book == NULL || output == NULL) {
        fprintf(stderr, "ERROR: NULL parameter in order_book_flush\n");
        return;
    }
    #endif

    // Generate cancel acks for all bid orders
    for (int i = 0; i < book->num_bid_levels; i++) {
        order_t* order = book->bids[i].orders_head;
        int count = 0;
        while (order != NULL && count < MAX_ORDERS_AT_PRICE_LEVEL) {
            output_msg_t msg = make_cancel_ack_msg(book->symbol, order->user_id, order->user_order_id);
            output_buffer_add(output, &msg);
            order = order->next;
            count++;
        }
    }

    // Generate cancel acks for all ask orders
    for (int i = 0; i < book->num_ask_levels; i++) {
        order_t* order = book->asks[i].orders_head;
        int count = 0;
        while (order != NULL && count < MAX_ORDERS_AT_PRICE_LEVEL) {
            output_msg_t msg = make_cancel_ack_msg(book->symbol, order->user_id, order->user_order_id);
            output_buffer_add(output, &msg);
            order = order->next;
            count++;
        }
    }

    // Free all bid levels
    for (int i = 0; i < book->num_bid_levels; i++) {
        list_free_all(book->bids[i].orders_head, &book->pools->order_pool);
        book->bids[i].orders_head = NULL;
        book->bids[i].orders_tail = NULL;
        book->bids[i].total_quantity = 0;
    }
    book->num_bid_levels = 0;

    // Free all ask levels
    for (int i = 0; i < book->num_ask_levels; i++) {
        list_free_all(book->asks[i].orders_head, &book->pools->order_pool);
        book->asks[i].orders_head = NULL;
        book->asks[i].orders_tail = NULL;
        book->asks[i].total_quantity = 0;
    }
    book->num_ask_levels = 0;

    // Clear order map
    order_map_clear(&book->order_map, &book->pools->hash_entry_pool);

    // Check for TOB changes
    check_tob_changes(book, output);

    // Reset TOB tracking
    book->prev_best_bid_price = 0;
    book->prev_best_bid_qty = 0;
    book->prev_best_ask_price = 0;
    book->prev_best_ask_qty = 0;

    check_tob_changes(book, output);
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

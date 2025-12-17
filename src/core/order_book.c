#include "core/order_book.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ============================================================================
 * Memory Pool Implementation
 * ============================================================================ */

/**
 * Initialize order pool - called once at startup
 * O(n) initialization, but only happens once
 */
static void order_pool_init(order_pool_t* pool) {
    assert(pool != NULL && "NULL pool in order_pool_init");

    pool->free_count = MAX_ORDERS_IN_POOL;

    /* Initialize free list (all indices available) */
    for (uint32_t i = 0; i < MAX_ORDERS_IN_POOL; i++) {
        pool->free_list[i] = i;
    }

    /* Zero out statistics */
    pool->total_allocations = 0;
    pool->peak_usage = 0;
    pool->allocation_failures = 0;
    
    /* Postcondition */
    assert(pool->free_count == MAX_ORDERS_IN_POOL && "order_pool_init failed");
}

/**
 * Initialize all memory pools (PUBLIC API)
 */
void memory_pools_init(memory_pools_t* pools) {
    assert(pools != NULL && "NULL pools in memory_pools_init");
    order_pool_init(&pools->order_pool);
    assert(pools->order_pool.free_count == MAX_ORDERS_IN_POOL && 
           "memory_pools_init failed");
}

/**
 * Get memory pool statistics (PUBLIC API)
 */
void memory_pools_get_stats(const memory_pools_t* pools,
                            const order_book_t* book,
                            memory_pool_stats_t* stats) {
    assert(pools != NULL && "NULL pools in memory_pools_get_stats");
    assert(stats != NULL && "NULL stats in memory_pools_get_stats");

    stats->order_allocations = pools->order_pool.total_allocations;
    stats->order_peak_usage = pools->order_pool.peak_usage;
    stats->order_failures = pools->order_pool.allocation_failures;

    /* Hash table stats from order book */
    if (book != NULL) {
        stats->hash_count = book->order_map.count;
        stats->hash_tombstones = book->order_map.tombstone_count;
    } else {
        stats->hash_count = 0;
        stats->hash_tombstones = 0;
    }

    stats->total_memory_bytes =
        (MAX_ORDERS_IN_POOL * sizeof(order_t)) +
        (ORDER_MAP_SIZE * sizeof(order_map_slot_t));
}

/**
 * Allocate order from pool (replaces malloc)
 * Performance: O(1), ~5-10 CPU cycles
 */
static inline order_t* order_pool_alloc(order_pool_t* pool) {
    assert(pool != NULL && "NULL pool in order_pool_alloc");
    assert(pool->free_count <= MAX_ORDERS_IN_POOL && "Invalid free_count");

    if (pool->free_count == 0) {
        /* Pool exhausted - critical error */
        pool->allocation_failures++;
        #ifdef DEBUG
        fprintf(stderr, "ERROR: Order pool exhausted! Consider increasing MAX_ORDERS_IN_POOL\n");
        #endif
        return NULL;
    }

    /* Pop from free list */
    pool->free_count--;
    uint32_t index = pool->free_list[pool->free_count];

    /* Rule 5: Bounds check assertion */
    assert(index < MAX_ORDERS_IN_POOL && "Invalid pool index");

    /* Update statistics */
    pool->total_allocations++;
    uint32_t current_usage = MAX_ORDERS_IN_POOL - pool->free_count;
    if (current_usage > pool->peak_usage) {
        pool->peak_usage = current_usage;
    }

    /* Return pointer to pre-allocated order */
    return &pool->orders[index];
}

/**
 * Free order back to pool (replaces free)
 * Performance: O(1), ~5-10 CPU cycles
 */
static inline void order_pool_free(order_pool_t* pool, order_t* order) {
    assert(pool != NULL && "NULL pool in order_pool_free");
    assert(order != NULL && "NULL order in order_pool_free");

    /* Validate that order is actually from our pool */
    assert(order >= pool->orders && "Order pointer below pool");
    assert(order < pool->orders + MAX_ORDERS_IN_POOL && "Order pointer above pool");

    /* Calculate index from pointer arithmetic */
    uint32_t index = (uint32_t)(order - pool->orders);

    /* Rule 5: Check for double-free */
    assert(pool->free_count < MAX_ORDERS_IN_POOL && "Pool overflow - possible double-free");

    /* Push to free list */
    pool->free_list[pool->free_count] = index;
    pool->free_count++;
}

/* ============================================================================
 * Open-Addressing Hash Table
 * ============================================================================
 *
 * Design:
 * - Linear probing for cache locality (sequential memory access)
 * - Power-of-2 size for fast masking instead of modulo
 * - Tombstones for deletion (UINT64_MAX)
 * - Empty slots marked with 0 (invalid order key)
 * ============================================================================ */

/**
 * Initialize hash map - zero all slots
 */
static void order_map_init(order_map_t* map) {
    assert(map != NULL && "NULL map in order_map_init");

    memset(map->slots, 0, sizeof(map->slots));
    map->count = 0;
    map->tombstone_count = 0;
    
    assert(map->count == 0 && map->tombstone_count == 0 && "order_map_init failed");
}

/**
 * Insert into order map using linear probing
 * Returns true on success, false if table is full
 */
static bool order_map_insert(order_map_t* map, uint64_t key,
                             const order_location_t* location) {
    assert(map != NULL && "NULL map in order_map_insert");
    assert(location != NULL && "NULL location in order_map_insert");
    assert(key != HASH_SLOT_EMPTY && "Cannot insert empty key");
    assert(key != HASH_SLOT_TOMBSTONE && "Cannot insert tombstone key");

    uint32_t hash = hash_order_key(key);

    /* Rule 2: Bounded probe sequence */
    for (uint32_t i = 0; i < MAX_PROBE_LENGTH; i++) {
        uint32_t idx = (hash + i) & ORDER_MAP_MASK;
        order_map_slot_t* slot = &map->slots[idx];

        /* Found empty or tombstone slot - insert here */
        if (slot->key == HASH_SLOT_EMPTY || slot->key == HASH_SLOT_TOMBSTONE) {
            if (slot->key == HASH_SLOT_TOMBSTONE) {
                map->tombstone_count--;  /* Reusing tombstone */
            }
            slot->key = key;
            slot->location = *location;
            map->count++;
            return true;
        }

        /* Key already exists - update (shouldn't happen for orders) */
        if (slot->key == key) {
            slot->location = *location;
            return true;
        }
    }

    /* Table is full or too many collisions */
    #ifdef DEBUG
    fprintf(stderr, "ERROR: Hash table probe limit exceeded (count=%u)\n", map->count);
    #endif
    return false;
}

/**
 * Find in order map using linear probing
 * Returns pointer to slot if found, NULL if not found
 */
static order_map_slot_t* order_map_find(order_map_t* map, uint64_t key) {
    assert(map != NULL && "NULL map in order_map_find");
    assert(map->count <= ORDER_MAP_SIZE && "Invalid map count");

    if (key == HASH_SLOT_EMPTY || key == HASH_SLOT_TOMBSTONE) {
        return NULL;
    }

    uint32_t hash = hash_order_key(key);

    /* Rule 2: Bounded probe sequence */
    for (uint32_t i = 0; i < MAX_PROBE_LENGTH; i++) {
        uint32_t idx = (hash + i) & ORDER_MAP_MASK;
        order_map_slot_t* slot = &map->slots[idx];

        /* Found it */
        if (slot->key == key) {
            return slot;
        }

        /* Hit empty slot - key doesn't exist */
        if (slot->key == HASH_SLOT_EMPTY) {
            return NULL;
        }

        /* Tombstone - keep probing */
    }

    return NULL;
}

/**
 * Remove from order map using tombstone
 * Returns true if found and removed, false if not found
 */
static bool order_map_remove(order_map_t* map, uint64_t key) {
    assert(map != NULL && "NULL map in order_map_remove");
    assert(map->count <= ORDER_MAP_SIZE && "Invalid map count");

    order_map_slot_t* slot = order_map_find(map, key);

    if (slot != NULL) {
        slot->key = HASH_SLOT_TOMBSTONE;
        map->count--;
        map->tombstone_count++;
        return true;
    }

    return false;
}

/**
 * Clear entire order map
 */
static void order_map_clear(order_map_t* map) {
    assert(map != NULL && "NULL map in order_map_clear");

    memset(map->slots, 0, sizeof(map->slots));
    map->count = 0;
    map->tombstone_count = 0;
    
    assert(map->count == 0 && "order_map_clear failed");
}

/* ============================================================================
 * Helper Functions - Doubly-Linked List Operations
 * ============================================================================ */

/**
 * Add order to end of list (FIFO for time priority)
 */
static void list_append(order_t** head, order_t** tail, order_t* order) {
    assert(head != NULL && "NULL head in list_append");
    assert(tail != NULL && "NULL tail in list_append");
    assert(order != NULL && "NULL order in list_append");

    order->next = NULL;
    order->prev = *tail;

    /* Rule 9: Avoid multi-level dereference - use temp variable */
    order_t* old_tail = *tail;
    if (old_tail != NULL) {
        old_tail->next = order;
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
    assert(head != NULL && "NULL head in list_remove");
    assert(tail != NULL && "NULL tail in list_remove");
    assert(order != NULL && "NULL order in list_remove");

    /* Rule 9: Use temp variables to avoid multi-level dereference */
    order_t* prev_order = order->prev;
    order_t* next_order = order->next;

    if (prev_order != NULL) {
        prev_order->next = next_order;
    } else {
        *head = next_order;
    }

    if (next_order != NULL) {
        next_order->prev = prev_order;
    } else {
        *tail = prev_order;
    }

    order->next = NULL;
    order->prev = NULL;
}

/**
 * Free all orders in a list (returns them to pool)
 */
static void list_free_all(order_t* head, order_pool_t* pool) {
    assert(pool != NULL && "NULL pool in list_free_all");

    order_t* current = head;

    /* Rule 2: Bounded loop */
    uint32_t count = 0;

    while (current != NULL && count < MAX_ORDERS_IN_POOL) {
        order_t* next = current->next;
        order_pool_free(pool, current);
        current = next;
        count++;
    }

    /* Rule 5: Assertion to catch infinite loop */
    assert(count < MAX_ORDERS_IN_POOL && "list_free_all hit iteration limit");
}

/* ============================================================================
 * Helper Functions - Price Level Management
 * ============================================================================ */

/**
 * Find price level in sorted array (binary search)
 * Returns index if found, -1 if not found
 */
static int find_price_level(const price_level_t* levels, uint32_t num_levels,
                             uint32_t price, bool descending) {
    assert(levels != NULL && "NULL levels in find_price_level");
    assert(num_levels <= MAX_PRICE_LEVELS && "num_levels exceeds maximum");

    if (num_levels == 0) {
        return -1;
    }

    int left = 0;
    int right = (int)num_levels - 1;

    /* Rule 2: Binary search is naturally bounded by log(num_levels) */
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
 * Returns index where level was inserted
 */
static int insert_price_level(price_level_t* levels, uint32_t* num_levels,
                               uint32_t price, bool descending) {
    assert(levels != NULL && "NULL levels in insert_price_level");
    assert(num_levels != NULL && "NULL num_levels in insert_price_level");
    assert(*num_levels < MAX_PRICE_LEVELS && "Price levels at capacity");

    /* Rule 2: Bounded by MAX_PRICE_LEVELS */
    uint32_t insert_pos = *num_levels;

    for (uint32_t i = 0; i < *num_levels && i < MAX_PRICE_LEVELS; i++) {
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

    /* Shift elements to make room */
    if (insert_pos < *num_levels) {
        memmove(&levels[insert_pos + 1], &levels[insert_pos],
                (size_t)(*num_levels - insert_pos) * sizeof(price_level_t));
    }

    /* Initialize new level */
    memset(&levels[insert_pos], 0, sizeof(price_level_t));
    levels[insert_pos].price = price;
    levels[insert_pos].total_quantity = 0;
    levels[insert_pos].orders_head = NULL;
    levels[insert_pos].orders_tail = NULL;
    levels[insert_pos].active = true;

    (*num_levels)++;

    return (int)insert_pos;
}

/**
 * Remove price level from sorted array
 */
static void remove_price_level(price_level_t* levels, uint32_t* num_levels,
                                uint32_t index, order_pool_t* pool) {
    assert(levels != NULL && "NULL levels in remove_price_level");
    assert(num_levels != NULL && "NULL num_levels in remove_price_level");
    assert(pool != NULL && "NULL pool in remove_price_level");

    if (index >= *num_levels) {
        return;
    }

    /* Free all orders at this level */
    list_free_all(levels[index].orders_head, pool);

    /* Shift elements down */
    if (index < *num_levels - 1) {
        memmove(&levels[index], &levels[index + 1],
                (size_t)(*num_levels - index - 1) * sizeof(price_level_t));
    }

    (*num_levels)--;
}

/* ============================================================================
 * Matching Engine - Trade Execution
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
    assert(book != NULL && "NULL book in execute_trade");
    assert(aggressor != NULL && "NULL aggressor in execute_trade");
    assert(passive != NULL && "NULL passive in execute_trade");
    assert(level != NULL && "NULL level in execute_trade");
    assert(output != NULL && "NULL output in execute_trade");

    /* Calculate trade quantity */
    uint32_t trade_qty = (aggressor->remaining_qty < passive->remaining_qty) ?
                         aggressor->remaining_qty : passive->remaining_qty;

    /* Rule 5: Assert valid trade */
    assert(trade_qty > 0 && "Zero-quantity trade");

    /* Determine buy/sell sides based on aggressor */
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

    /* Generate trade message */
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

    /* Update quantities */
    order_fill(aggressor, trade_qty);
    order_fill(passive, trade_qty);
    level->total_quantity -= trade_qty;

    /* Check if passive order is fully filled */
    bool passive_filled = order_is_filled(passive);

    if (passive_filled) {
        uint64_t key = make_order_key(passive->user_id, passive->user_order_id);
        order_map_remove(&book->order_map, key);
        list_remove(&level->orders_head, &level->orders_tail, passive);
        order_pool_free(&book->pools->order_pool, passive);
    }

    return passive_filled;
}

/* ============================================================================
 * Matching Engine - Buy Side Matcher
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
    assert(book != NULL && "NULL book in match_buy_order");
    assert(order != NULL && "NULL order in match_buy_order");
    assert(order->side == SIDE_BUY && "Non-buy order in match_buy_order");
    assert(output != NULL && "NULL output in match_buy_order");

    /* Rule 2: Fixed upper bound on iterations */
    uint32_t iteration_count = 0;

    /* Match against asks (best ask = lowest price = index 0) */
    while (order->remaining_qty > 0 &&
           book->num_ask_levels > 0 &&
           iteration_count < MAX_MATCH_ITERATIONS) {

        iteration_count++;

        price_level_t* best_ask_level = &book->asks[0];

        /* Check if we can match at this price */
        bool can_match = (order->type == ORDER_TYPE_MARKET) ||
                        (order->price >= best_ask_level->price);

        if (!can_match) {
            break;
        }

        /* Match with orders at this price level (FIFO) */
        order_t* passive_order = best_ask_level->orders_head;

        /* Rule 2: Bound the inner loop */
        uint32_t inner_iteration = 0;

        while (order->remaining_qty > 0 &&
               passive_order != NULL &&
               inner_iteration < MAX_ORDERS_AT_PRICE_LEVEL) {

            inner_iteration++;
            order_t* next_order = passive_order->next;

            /* Execute the trade */
            execute_trade(book, order, passive_order, best_ask_level,
                         best_ask_level->price, output);

            passive_order = next_order;
        }

        /* Remove price level if empty */
        if (best_ask_level->orders_head == NULL) {
            remove_price_level(book->asks, &book->num_ask_levels, 0,
                             &book->pools->order_pool);
        }
    }

    /* Rule 5: Assertion to catch runaway matching */
    assert(iteration_count < MAX_MATCH_ITERATIONS && "Buy order matching hit iteration limit");
}

/* ============================================================================
 * Matching Engine - Sell Side Matcher
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
    assert(book != NULL && "NULL book in match_sell_order");
    assert(order != NULL && "NULL order in match_sell_order");
    assert(order->side == SIDE_SELL && "Non-sell order in match_sell_order");
    assert(output != NULL && "NULL output in match_sell_order");

    /* Rule 2: Fixed upper bound on iterations */
    uint32_t iteration_count = 0;

    /* Match against bids (best bid = highest price = index 0) */
    while (order->remaining_qty > 0 &&
           book->num_bid_levels > 0 &&
           iteration_count < MAX_MATCH_ITERATIONS) {

        iteration_count++;

        price_level_t* best_bid_level = &book->bids[0];

        /* Check if we can match at this price */
        bool can_match = (order->type == ORDER_TYPE_MARKET) ||
                        (order->price <= best_bid_level->price);

        if (!can_match) {
            break;
        }

        /* Match with orders at this price level (FIFO) */
        order_t* passive_order = best_bid_level->orders_head;

        /* Rule 2: Bound the inner loop */
        uint32_t inner_iteration = 0;

        while (order->remaining_qty > 0 &&
               passive_order != NULL &&
               inner_iteration < MAX_ORDERS_AT_PRICE_LEVEL) {

            inner_iteration++;
            order_t* next_order = passive_order->next;

            /* Execute the trade */
            execute_trade(book, order, passive_order, best_bid_level,
                         best_bid_level->price, output);

            passive_order = next_order;
        }

        /* Remove price level if empty */
        if (best_bid_level->orders_head == NULL) {
            remove_price_level(book->bids, &book->num_bid_levels, 0,
                             &book->pools->order_pool);
        }
    }

    /* Rule 5: Assertion to catch runaway matching */
    assert(iteration_count < MAX_MATCH_ITERATIONS && "Sell order matching hit iteration limit");
}

/* ============================================================================
 * Matching Engine - Dispatcher
 * ============================================================================ */

/**
 * Match an incoming order against the book
 * Dispatches to side-specific matcher
 */
static inline void match_order(
    order_book_t* book,
    order_t* order,
    output_buffer_t* output)
{
    assert(book != NULL && "NULL book in match_order");
    assert(order != NULL && "NULL order in match_order");
    
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
    assert(book != NULL && "NULL book in add_order_to_level");
    assert(level != NULL && "NULL level in add_order_to_level");
    assert(order != NULL && "NULL order in add_order_to_level");

    /* Add to list */
    list_append(&level->orders_head, &level->orders_tail, order);

    /* Update total quantity */
    level->total_quantity += order->remaining_qty;

    /* Add to order map for cancellation */
    uint64_t key = make_order_key(order->user_id, order->user_order_id);
    order_location_t location;
    location.side = order->side;
    location.price = order->price;
    location.order_ptr = order;

    bool inserted = order_map_insert(&book->order_map, key, &location);
    assert(inserted && "Failed to insert into order map");
    (void)inserted;  /* Suppress unused warning in release */
}

/**
 * Add order to book (for limit orders that don't fully match)
 */
static void add_to_book(order_book_t* book, order_t* order) {
    assert(book != NULL && "NULL book in add_to_book");
    assert(order != NULL && "NULL order in add_to_book");
    assert(order->type == ORDER_TYPE_LIMIT && "Adding non-limit order to book");
    assert(order->price > 0 && "Limit order with zero price");

    if (order->side == SIDE_BUY) {
        int idx = find_price_level(book->bids, book->num_bid_levels, order->price, true);

        if (idx == -1) {
            idx = insert_price_level(book->bids, &book->num_bid_levels, order->price, true);
        }

        assert(idx >= 0 && (uint32_t)idx < book->num_bid_levels && "Invalid bid level index");
        add_order_to_level(book, &book->bids[idx], order);
    } else {
        int idx = find_price_level(book->asks, book->num_ask_levels, order->price, false);

        if (idx == -1) {
            idx = insert_price_level(book->asks, &book->num_ask_levels, order->price, false);
        }

        assert(idx >= 0 && (uint32_t)idx < book->num_ask_levels && "Invalid ask level index");
        add_order_to_level(book, &book->asks[idx], order);
    }
}

/**
 * Check for top-of-book changes and generate TOB messages
 */
static void check_tob_changes(order_book_t* book, output_buffer_t* output) {
    assert(book != NULL && "NULL book in check_tob_changes");
    assert(output != NULL && "NULL output in check_tob_changes");
    
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
    assert(book != NULL && "NULL book in order_book_init");
    assert(symbol != NULL && "NULL symbol in order_book_init");
    assert(pools != NULL && "NULL pools in order_book_init");

    /* Safe string copy with length check */
    size_t len = strnlen(symbol, MAX_SYMBOL_LENGTH);
    assert(len < MAX_SYMBOL_LENGTH && "Symbol too long - would be truncated");
    memcpy(book->symbol, symbol, len);
    book->symbol[len] = '\0';

    /* Initialize price levels */
    memset(book->bids, 0, sizeof(book->bids));
    memset(book->asks, 0, sizeof(book->asks));
    book->num_bid_levels = 0;
    book->num_ask_levels = 0;

    /* Initialize order map (open-addressing) */
    order_map_init(&book->order_map);

    /* Initialize TOB tracking */
    book->prev_best_bid_price = 0;
    book->prev_best_bid_qty = 0;
    book->prev_best_ask_price = 0;
    book->prev_best_ask_qty = 0;
    book->bid_side_ever_active = false;
    book->ask_side_ever_active = false;

    /* Initialize flush state */
    memset(&book->flush_state, 0, sizeof(book->flush_state));

    /* Store reference to memory pools */
    book->pools = pools;
}

/**
 * Destroy order book and return all memory to pools
 */
void order_book_destroy(order_book_t* book) {
    assert(book != NULL && "NULL book in order_book_destroy");
    assert(book->pools != NULL && "NULL pools in order_book_destroy");

    /* Free all bid levels */
    for (uint32_t i = 0; i < book->num_bid_levels; i++) {
        list_free_all(book->bids[i].orders_head, &book->pools->order_pool);
    }

    /* Free all ask levels */
    for (uint32_t i = 0; i < book->num_ask_levels; i++) {
        list_free_all(book->asks[i].orders_head, &book->pools->order_pool);
    }

    /* Clear order map */
    order_map_clear(&book->order_map);
}

/**
 * Process new order
 */
void order_book_add_order(order_book_t* book,
                          const new_order_msg_t* msg,
                          uint32_t client_id,
                          output_buffer_t* output) {
    assert(book != NULL && "NULL book in order_book_add_order");
    assert(msg != NULL && "NULL msg in order_book_add_order");
    assert(output != NULL && "NULL output in order_book_add_order");

    /* Allocate from pool instead of malloc */
    order_t* order = order_pool_alloc(&book->pools->order_pool);

    if (order == NULL) {
        /* Pool exhausted - log and return */
        #ifdef DEBUG
        fprintf(stderr, "ERROR: Order pool exhausted for %s (user %u, order %u)\n",
                book->symbol, msg->user_id, msg->user_order_id);
        #endif
        return;
    }

    /* Initialize order */
    uint64_t timestamp = order_get_current_timestamp();
    order_init(order, msg, timestamp);
    order->client_id = client_id;

    /* Send acknowledgement */
    output_msg_t ack = make_ack_msg(book->symbol, order->user_id, order->user_order_id);
    output_buffer_add(output, &ack);

    /* Try to match the order */
    match_order(book, order, output);

    /* If order has remaining quantity and is a limit order, add to book */
    if (order->remaining_qty > 0 && order->type == ORDER_TYPE_LIMIT) {
        add_to_book(book, order);
    } else {
        /* Order fully filled or market order - return to pool */
        order_pool_free(&book->pools->order_pool, order);
    }

    /* Check for top-of-book changes */
    check_tob_changes(book, output);
}

/**
 * Cancel order
 */
void order_book_cancel_order(order_book_t* book, uint32_t user_id, uint32_t user_order_id,
                              output_buffer_t* output) {
    assert(book != NULL && "NULL book in order_book_cancel_order");
    assert(output != NULL && "NULL output in order_book_cancel_order");

    uint64_t key = make_order_key(user_id, user_order_id);
    order_map_slot_t* slot = order_map_find(&book->order_map, key);

    if (slot == NULL) {
        /* Order not found - still send cancel ack */
        output_msg_t msg = make_cancel_ack_msg(book->symbol, user_id, user_order_id);
        output_buffer_add(output, &msg);
        return;
    }

    /* Get order location */
    order_location_t* loc = &slot->location;
    order_t* order = loc->order_ptr;

    /* Find price level */
    price_level_t* levels;
    uint32_t* num_levels;
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

        /* Update total quantity */
        level->total_quantity -= order->remaining_qty;

        /* Remove from list */
        list_remove(&level->orders_head, &level->orders_tail, order);

        /* Return to pool */
        order_pool_free(&book->pools->order_pool, order);

        /* Remove price level if empty */
        if (level->orders_head == NULL) {
            remove_price_level(levels, num_levels, (uint32_t)idx, &book->pools->order_pool);
        }
    }

    /* Remove from order map */
    order_map_remove(&book->order_map, key);

    /* Send cancel acknowledgement */
    output_msg_t msg = make_cancel_ack_msg(book->symbol, user_id, user_order_id);
    output_buffer_add(output, &msg);

    /* Check for top-of-book changes */
    check_tob_changes(book, output);
}

/**
 * Order key pair for batch cancellation
 * Used to avoid iterator invalidation when cancelling multiple orders
 */
typedef struct {
    uint32_t user_id;
    uint32_t user_order_id;
} order_key_pair_t;

/**
 * Cancel all orders for a specific client (TCP mode)
 * 
 * IMPORTANT: Uses two-phase approach to avoid iterator invalidation.
 * Phase 1: Collect all order keys for target client
 * Phase 2: Cancel orders by key (safe because we're not iterating the list)
 */
size_t order_book_cancel_client_orders(order_book_t* book,
                                       uint32_t client_id,
                                       output_buffer_t* output) {
    assert(book != NULL && "NULL book in order_book_cancel_client_orders");
    assert(output != NULL && "NULL output in order_book_cancel_client_orders");

    /* Phase 1: Collect keys to cancel (avoids iterator invalidation) */
    order_key_pair_t to_cancel[MAX_ORDERS_IN_POOL];
    uint32_t cancel_count = 0;

    /* Scan bid levels */
    for (uint32_t i = 0; i < book->num_bid_levels && i < MAX_PRICE_LEVELS; i++) {
        order_t* order = book->bids[i].orders_head;
        uint32_t order_count = 0;

        while (order != NULL && order_count < MAX_ORDERS_AT_PRICE_LEVEL) {
            if (order->client_id == client_id && cancel_count < MAX_ORDERS_IN_POOL) {
                to_cancel[cancel_count].user_id = order->user_id;
                to_cancel[cancel_count].user_order_id = order->user_order_id;
                cancel_count++;
            }
            order = order->next;
            order_count++;
        }
    }

    /* Scan ask levels */
    for (uint32_t i = 0; i < book->num_ask_levels && i < MAX_PRICE_LEVELS; i++) {
        order_t* order = book->asks[i].orders_head;
        uint32_t order_count = 0;

        while (order != NULL && order_count < MAX_ORDERS_AT_PRICE_LEVEL) {
            if (order->client_id == client_id && cancel_count < MAX_ORDERS_IN_POOL) {
                to_cancel[cancel_count].user_id = order->user_id;
                to_cancel[cancel_count].user_order_id = order->user_order_id;
                cancel_count++;
            }
            order = order->next;
            order_count++;
        }
    }

    /* Phase 2: Cancel collected orders (safe - not iterating list) */
    for (uint32_t i = 0; i < cancel_count; i++) {
        order_book_cancel_order(book, to_cancel[i].user_id, 
                                to_cancel[i].user_order_id, output);
    }

    return (size_t)cancel_count;
}

/**
 * Reset flush state
 */
void order_book_flush_reset(order_book_t* book) {
    assert(book != NULL && "NULL book in order_book_flush_reset");
    memset(&book->flush_state, 0, sizeof(book->flush_state));
    assert(!book->flush_state.in_progress && "flush_reset failed");
}

/* ============================================================================
 * Flush Implementation - Split into Sub-functions (Rule 4)
 * ============================================================================ */

/**
 * Process one side (bids or asks) during flush
 * Returns number of orders processed
 */
static uint32_t flush_process_side(
    order_book_t* book,
    output_buffer_t* output,
    price_level_t* levels,
    uint32_t num_levels,
    uint32_t* current_level,
    order_t** current_order,
    bool* side_done,
    uint32_t budget)
{
    assert(book != NULL && "NULL book in flush_process_side");
    assert(output != NULL && "NULL output in flush_process_side");
    
    uint32_t processed = 0;

    while (!(*side_done) && processed < budget) {
        /* Check if we've processed all levels */
        if (*current_level >= num_levels) {
            *side_done = true;
            break;
        }

        price_level_t* level = &levels[*current_level];

        /* Get current order (or start of level) */
        if (*current_order == NULL) {
            *current_order = level->orders_head;
        }

        /* Process orders at this level */
        while (*current_order != NULL && processed < budget) {
            order_t* order = *current_order;
            *current_order = order->next;

            /* Generate cancel ack */
            output_msg_t msg = make_cancel_ack_msg(book->symbol, order->user_id, order->user_order_id);
            output_buffer_add(output, &msg);
            processed++;
        }

        /* Move to next level if this one is done */
        if (*current_order == NULL) {
            (*current_level)++;
        }
    }

    return processed;
}

/**
 * Finalize flush - clean up data structures and emit TOB messages
 */
static void flush_finalize(order_book_t* book, output_buffer_t* output) {
    assert(book != NULL && "NULL book in flush_finalize");
    assert(output != NULL && "NULL output in flush_finalize");

    /* Free all bid levels */
    for (uint32_t i = 0; i < book->num_bid_levels; i++) {
        list_free_all(book->bids[i].orders_head, &book->pools->order_pool);
        book->bids[i].orders_head = NULL;
        book->bids[i].orders_tail = NULL;
        book->bids[i].total_quantity = 0;
    }
    book->num_bid_levels = 0;

    /* Free all ask levels */
    for (uint32_t i = 0; i < book->num_ask_levels; i++) {
        list_free_all(book->asks[i].orders_head, &book->pools->order_pool);
        book->asks[i].orders_head = NULL;
        book->asks[i].orders_tail = NULL;
        book->asks[i].total_quantity = 0;
    }
    book->num_ask_levels = 0;

    /* Clear order map */
    order_map_clear(&book->order_map);

    /* Force TOB "eliminated" messages */
    if (book->bid_side_ever_active) {
        book->prev_best_bid_price = 1;  /* Force change detection */
        book->prev_best_bid_qty = 1;
    }
    if (book->ask_side_ever_active) {
        book->prev_best_ask_price = 1;  /* Force change detection */
        book->prev_best_ask_qty = 1;
    }

    /* Emit TOB eliminated messages */
    check_tob_changes(book, output);

    /* Reset tracking state */
    book->prev_best_bid_price = 0;
    book->prev_best_bid_qty = 0;
    book->prev_best_ask_price = 0;
    book->prev_best_ask_qty = 0;
    book->bid_side_ever_active = false;
    book->ask_side_ever_active = false;

    /* Reset flush state */
    memset(&book->flush_state, 0, sizeof(book->flush_state));
}

/**
 * Flush/clear the entire order book - ITERATIVE VERSION
 *
 * Processes up to FLUSH_BATCH_SIZE orders per call.
 * Returns true when flush is complete, false if more iterations needed.
 */
bool order_book_flush(order_book_t* book, output_buffer_t* output) {
    assert(book != NULL && "NULL book in order_book_flush");
    assert(output != NULL && "NULL output in order_book_flush");

    flush_state_t* state = &book->flush_state;

    /* Initialize flush state on first call */
    if (!state->in_progress) {
        state->in_progress = true;
        state->current_bid_level = 0;
        state->current_ask_level = 0;
        state->current_order = NULL;
        state->processing_bids = true;
        state->bids_done = false;
        state->asks_done = false;
    }

    uint32_t orders_processed = 0;
    uint32_t remaining_budget = FLUSH_BATCH_SIZE;

    /* Process bids */
    if (!state->bids_done) {
        uint32_t bid_processed = flush_process_side(
            book, output,
            book->bids, book->num_bid_levels,
            &state->current_bid_level,
            &state->current_order,
            &state->bids_done,
            remaining_budget
        );
        orders_processed += bid_processed;
        remaining_budget -= bid_processed;
        
        /* Reset current_order when switching sides */
        if (state->bids_done) {
            state->current_order = NULL;
        }
    }

    /* Process asks */
    if (!state->asks_done && remaining_budget > 0) {
        uint32_t ask_processed = flush_process_side(
            book, output,
            book->asks, book->num_ask_levels,
            &state->current_ask_level,
            &state->current_order,
            &state->asks_done,
            remaining_budget
        );
        orders_processed += ask_processed;
    }

    (void)orders_processed;  /* May be unused if not debugging */

    /* Check if flush is complete */
    if (state->bids_done && state->asks_done) {
        flush_finalize(book, output);
        return true;  /* Flush complete */
    }

    return false;  /* More iterations needed */
}

/**
 * Get best bid price (0 if none)
 */
uint32_t order_book_get_best_bid_price(const order_book_t* book) {
    assert(book != NULL && "NULL book in order_book_get_best_bid_price");
    assert(book->num_bid_levels <= MAX_PRICE_LEVELS && "Invalid num_bid_levels");
    return (book->num_bid_levels > 0) ? book->bids[0].price : 0;
}

/**
 * Get best ask price (0 if none)
 */
uint32_t order_book_get_best_ask_price(const order_book_t* book) {
    assert(book != NULL && "NULL book in order_book_get_best_ask_price");
    assert(book->num_ask_levels <= MAX_PRICE_LEVELS && "Invalid num_ask_levels");
    return (book->num_ask_levels > 0) ? book->asks[0].price : 0;
}

/**
 * Get total quantity at best bid
 */
uint32_t order_book_get_best_bid_quantity(const order_book_t* book) {
    assert(book != NULL && "NULL book in order_book_get_best_bid_quantity");
    assert(book->num_bid_levels <= MAX_PRICE_LEVELS && "Invalid num_bid_levels");
    return (book->num_bid_levels > 0) ? book->bids[0].total_quantity : 0;
}

/**
 * Get total quantity at best ask
 */
uint32_t order_book_get_best_ask_quantity(const order_book_t* book) {
    assert(book != NULL && "NULL book in order_book_get_best_ask_quantity");
    assert(book->num_ask_levels <= MAX_PRICE_LEVELS && "Invalid num_ask_levels");
    return (book->num_ask_levels > 0) ? book->asks[0].total_quantity : 0;
}

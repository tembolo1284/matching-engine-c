#ifndef MATCHING_ENGINE_ORDER_H
#define MATCHING_ENGINE_ORDER_H

#include "protocol/message_types.h"
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * order_t - Represents a single order in the order book
 * 
 * Design decisions:
 * - Uses uint64_t timestamp for time priority (nanoseconds since epoch)
 * - Tracks remaining_qty separately for partial fills
 * - Stores both user_id and user_order_id for cancel operations
 * - Fixed-size symbol array (no dynamic allocation)
 */
typedef struct order {
    /* Order identification */
    uint32_t user_id;
    uint32_t user_order_id;
    char symbol[MAX_SYMBOL_LENGTH];
    
    /* Order details */
    uint32_t price;           /* 0 = market order, >0 = limit order */
    uint32_t quantity;        /* Original quantity */
    uint32_t remaining_qty;   /* Remaining unfilled quantity */
    side_t side;
    order_type_t type;
    
    /* Time priority (nanoseconds since epoch) */
    uint64_t timestamp;
    
    /* For linked list (utlist) - will add these */
    struct order *next;
    struct order *prev;
} order_t;

/**
 * Initialize order from new order message
 */
static inline void order_init(order_t* order, const new_order_msg_t* msg, uint64_t timestamp) {
    order->user_id = msg->user_id;
    order->user_order_id = msg->user_order_id;
    strncpy(order->symbol, msg->symbol, MAX_SYMBOL_LENGTH - 1);
    order->symbol[MAX_SYMBOL_LENGTH - 1] = '\0';  /* Ensure null termination */
    
    order->price = msg->price;
    order->quantity = msg->quantity;
    order->remaining_qty = msg->quantity;
    order->side = msg->side;
    order->type = (msg->price == 0) ? ORDER_TYPE_MARKET : ORDER_TYPE_LIMIT;
    
    order->timestamp = timestamp;
    order->next = NULL;
    order->prev = NULL;
}

/**
 * Check if order is fully filled
 */
static inline bool order_is_filled(const order_t* order) {
    return order->remaining_qty == 0;
}

/**
 * Fill order by quantity, returns amount actually filled
 */
static inline uint32_t order_fill(order_t* order, uint32_t qty) {
    uint32_t filled = (qty < order->remaining_qty) ? qty : order->remaining_qty;
    order->remaining_qty -= filled;
    return filled;
}

/**
 * Get current timestamp in nanoseconds (replaces std::chrono)
 */
static inline uint64_t order_get_current_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_ORDER_H */

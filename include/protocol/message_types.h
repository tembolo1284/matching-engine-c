#ifndef MATCHING_ENGINE_MESSAGE_TYPES_H
#define MATCHING_ENGINE_MESSAGE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_SYMBOL_LENGTH 16

/* ============================================================================
 * Enumerations
 * ============================================================================ */

typedef enum {
    SIDE_BUY  = 'B',
    SIDE_SELL = 'S'
} side_t;

typedef enum {
    ORDER_TYPE_MARKET,  /* price = 0 */
    ORDER_TYPE_LIMIT    /* price > 0 */
} order_type_t;

typedef enum {
    INPUT_MSG_NEW_ORDER,
    INPUT_MSG_CANCEL,
    INPUT_MSG_FLUSH
} input_msg_type_t;

typedef enum {
    OUTPUT_MSG_ACK,          /* A, userId, userOrderId */
    OUTPUT_MSG_CANCEL_ACK,   /* C, userId, userOrderId */
    OUTPUT_MSG_TRADE,        /* T, userIdBuy, userOrderIdBuy, userIdSell, userOrderIdSell, price, quantity */
    OUTPUT_MSG_TOP_OF_BOOK   /* B, side (B or S), price, totalQuantity */
} output_msg_type_t;

/* ============================================================================
 * Input Message Structures
 * ============================================================================ */

typedef struct {
    uint32_t user_id;
    char symbol[MAX_SYMBOL_LENGTH];
    uint32_t price;         /* 0 = market order */
    uint32_t quantity;
    side_t side;
    uint32_t user_order_id;
} new_order_msg_t;

typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];
    uint32_t user_id;
    uint32_t user_order_id;
} cancel_msg_t;

typedef struct {
    /* Empty - just signals flush */
    char padding;  /* Prevent empty struct (not valid C) */
} flush_msg_t;

/* Input message - tagged union (replaces std::variant) */
typedef struct {
    input_msg_type_t type;
    union {
        new_order_msg_t new_order;
        cancel_msg_t cancel;
        flush_msg_t flush;
    } data;
} input_msg_t;

/* ============================================================================
 * Output Message Structures
 * ============================================================================ */

typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];
    uint32_t user_id;
    uint32_t user_order_id;
} ack_msg_t;

typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];
    uint32_t user_id;
    uint32_t user_order_id;
} cancel_ack_msg_t;

typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];
    uint32_t user_id_buy;
    uint32_t user_order_id_buy;
    uint32_t user_id_sell;
    uint32_t user_order_id_sell;
    uint32_t price;
    uint32_t quantity;

    uint32_t buy_client_id;
    uint32_t sell_client_id;
} trade_msg_t;

typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];
    side_t side;
    uint32_t price;          /* 0 means no price (eliminated) */
    uint32_t total_quantity; /* 0 means eliminated */
    bool eliminated;         /* True when side is eliminated (use "-, -") */
} top_of_book_msg_t;

/* Output message - tagged union (replaces std::variant) */
typedef struct {
    output_msg_type_t type;
    union {
        ack_msg_t ack;
        cancel_ack_msg_t cancel_ack;
        trade_msg_t trade;
        top_of_book_msg_t top_of_book;
    } data;
} output_msg_t;

/* ============================================================================
 * Helper Functions (replaces C++ static factory methods)
 * ============================================================================ */

/* Create input messages */
static inline input_msg_t make_new_order_msg(const new_order_msg_t* msg) {
    input_msg_t result;
    result.type = INPUT_MSG_NEW_ORDER;
    result.data.new_order = *msg;
    return result;
}

static inline input_msg_t make_cancel_msg(const cancel_msg_t* msg) {
    input_msg_t result;
    result.type = INPUT_MSG_CANCEL;
    result.data.cancel = *msg;
    return result;
}

static inline input_msg_t make_flush_msg(void) {
    input_msg_t result;
    result.type = INPUT_MSG_FLUSH;
    result.data.flush.padding = 0;
    return result;
}

/* Create output messages */
static inline output_msg_t make_ack_msg(const char* symbol, uint32_t user_id, uint32_t user_order_id) {
    output_msg_t msg;
    msg.type = OUTPUT_MSG_ACK;
    strncpy(msg.data.ack.symbol, symbol, MAX_SYMBOL_LENGTH - 1);
    msg.data.ack.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
    msg.data.ack.user_id = user_id;
    msg.data.ack.user_order_id = user_order_id;
    return msg;
}

static inline output_msg_t make_cancel_ack_msg(const char* symbol, uint32_t user_id, uint32_t user_order_id) {
    output_msg_t msg;
    msg.type = OUTPUT_MSG_CANCEL_ACK;
    strncpy(msg.data.cancel_ack.symbol, symbol, MAX_SYMBOL_LENGTH - 1);
    msg.data.cancel_ack.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
    msg.data.cancel_ack.user_id = user_id;
    msg.data.cancel_ack.user_order_id = user_order_id;
    return msg;
}

static inline output_msg_t make_trade_msg(const char* symbol,
                                          uint32_t user_id_buy, uint32_t user_order_id_buy,
                                          uint32_t user_id_sell, uint32_t user_order_id_sell,
                                          uint32_t price, uint32_t quantity) {
    output_msg_t msg;
    msg.type = OUTPUT_MSG_TRADE;
    strncpy(msg.data.trade.symbol, symbol, MAX_SYMBOL_LENGTH - 1);
    msg.data.trade.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
    msg.data.trade.user_id_buy = user_id_buy;
    msg.data.trade.user_order_id_buy = user_order_id_buy;
    msg.data.trade.user_id_sell = user_id_sell;
    msg.data.trade.user_order_id_sell = user_order_id_sell;
    msg.data.trade.price = price;
    msg.data.trade.quantity = quantity;
    return msg;
}

static inline output_msg_t make_top_of_book_msg(const char* symbol, side_t side, uint32_t price, uint32_t total_quantity) {
    output_msg_t msg;
    msg.type = OUTPUT_MSG_TOP_OF_BOOK;
    strncpy(msg.data.top_of_book.symbol, symbol, MAX_SYMBOL_LENGTH - 1);
    msg.data.top_of_book.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
    msg.data.top_of_book.side = side;
    msg.data.top_of_book.price = price;
    msg.data.top_of_book.total_quantity = total_quantity;
    return msg;
}

static inline output_msg_t make_top_of_book_eliminated_msg(const char* symbol, side_t side) {
    output_msg_t msg;
    msg.type = OUTPUT_MSG_TOP_OF_BOOK;
    strncpy(msg.data.top_of_book.symbol, symbol, MAX_SYMBOL_LENGTH - 1);
    msg.data.top_of_book.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
    msg.data.top_of_book.side = side;
    msg.data.top_of_book.price = 0;
    msg.data.top_of_book.total_quantity = 0;
    return msg;
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_MESSAGE_TYPES_H */

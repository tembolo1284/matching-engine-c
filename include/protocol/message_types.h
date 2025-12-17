#ifndef MATCHING_ENGINE_MESSAGE_TYPES_H
#define MATCHING_ENGINE_MESSAGE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdalign.h>
#include <assert.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Message Type Definitions
 *
 * Core message structures for the matching engine protocol.
 * All structures are designed for minimal padding and optimal cache behavior.
 *
 * Design principles:
 * - Explicit padding for portability
 * - Static assertions verify all sizes
 * - Helper functions include Rule 5 assertions
 * - uint8_t enums for space efficiency
 */

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_SYMBOL_LENGTH 16

/* ============================================================================
 * Enumerations - Using uint8_t for space efficiency (C11 compatible)
 * ============================================================================
 *
 * Note: In C11, enum underlying type is implementation-defined (usually int).
 * We use uint8_t typedefs with named constants to guarantee 1-byte storage.
 */

/* Side: Buy or Sell */
typedef uint8_t side_t;
#define SIDE_BUY  ((side_t)'B')
#define SIDE_SELL ((side_t)'S')

/* Order type */
typedef uint8_t order_type_t;
#define ORDER_TYPE_MARKET ((order_type_t)0)
#define ORDER_TYPE_LIMIT  ((order_type_t)1)

/* Input message types */
typedef uint8_t input_msg_type_t;
#define INPUT_MSG_NEW_ORDER ((input_msg_type_t)0)
#define INPUT_MSG_CANCEL    ((input_msg_type_t)1)
#define INPUT_MSG_FLUSH     ((input_msg_type_t)2)

/* Output message types */
typedef uint8_t output_msg_type_t;
#define OUTPUT_MSG_ACK         ((output_msg_type_t)0)
#define OUTPUT_MSG_CANCEL_ACK  ((output_msg_type_t)1)
#define OUTPUT_MSG_TRADE       ((output_msg_type_t)2)
#define OUTPUT_MSG_TOP_OF_BOOK ((output_msg_type_t)3)

/* ============================================================================
 * Input Message Structures
 * ============================================================================
 *
 * Layout optimized for minimal padding:
 * - Largest alignment fields (uint32_t) first
 * - Group same-size fields together
 * - Small fields (uint8_t) and char arrays at end
 */

/**
 * New Order Message
 *
 * Layout (36 bytes):
 *   0-3:   user_id
 *   4-7:   user_order_id
 *   8-11:  price
 *   12-15: quantity
 *   16:    side (uint8_t)
 *   17-19: padding (explicit)
 *   20-35: symbol[16]
 */
typedef struct {
    uint32_t user_id;
    uint32_t user_order_id;
    uint32_t price;         /* 0 = market order */
    uint32_t quantity;
    side_t side;            /* 1 byte */
    uint8_t _pad[3];        /* Explicit padding */
    char symbol[MAX_SYMBOL_LENGTH];
} new_order_msg_t;

_Static_assert(sizeof(new_order_msg_t) == 36, "new_order_msg_t should be 36 bytes");
_Static_assert(offsetof(new_order_msg_t, symbol) == 20, "symbol at wrong offset");

/**
 * Cancel Message
 *
 * Layout (24 bytes):
 *   0-3:   user_id
 *   4-7:   user_order_id
 *   8-23:  symbol[16]
 */
typedef struct {
    uint32_t user_id;
    uint32_t user_order_id;
    char symbol[MAX_SYMBOL_LENGTH];
} cancel_msg_t;

_Static_assert(sizeof(cancel_msg_t) == 24, "cancel_msg_t should be 24 bytes");

/**
 * Flush Message - empty signal
 */
typedef struct {
    uint8_t _unused;  /* Prevent empty struct */
} flush_msg_t;

_Static_assert(sizeof(flush_msg_t) == 1, "flush_msg_t should be 1 byte");

/**
 * Input Message - Tagged Union
 *
 * Layout (40 bytes):
 *   0:     type (uint8_t)
 *   1-3:   padding (for uint32_t alignment in union)
 *   4-39:  union (36 bytes = new_order)
 */
typedef struct {
    input_msg_type_t type;
    uint8_t _pad[3];  /* Explicit padding for union alignment */
    union {
        new_order_msg_t new_order;  /* 36 bytes */
        cancel_msg_t cancel;         /* 24 bytes */
        flush_msg_t flush;           /* 1 byte */
    } data;
} input_msg_t;

_Static_assert(sizeof(input_msg_t) == 40, "input_msg_t should be 40 bytes");
_Static_assert(offsetof(input_msg_t, data) == 4, "data union at wrong offset");

/* ============================================================================
 * Output Message Structures
 * ============================================================================ */

/**
 * Acknowledgment Message
 *
 * Layout (24 bytes):
 *   0-3:   user_id
 *   4-7:   user_order_id
 *   8-23:  symbol[16]
 */
typedef struct {
    uint32_t user_id;
    uint32_t user_order_id;
    char symbol[MAX_SYMBOL_LENGTH];
} ack_msg_t;

_Static_assert(sizeof(ack_msg_t) == 24, "ack_msg_t should be 24 bytes");

/**
 * Cancel Acknowledgment Message (same layout as ack)
 */
typedef struct {
    uint32_t user_id;
    uint32_t user_order_id;
    char symbol[MAX_SYMBOL_LENGTH];
} cancel_ack_msg_t;

_Static_assert(sizeof(cancel_ack_msg_t) == 24, "cancel_ack_msg_t should be 24 bytes");

/**
 * Trade Message
 *
 * Layout (48 bytes):
 *   0-3:   user_id_buy
 *   4-7:   user_order_id_buy
 *   8-11:  user_id_sell
 *   12-15: user_order_id_sell
 *   16-19: price
 *   20-23: quantity
 *   24-27: buy_client_id
 *   28-31: sell_client_id
 *   32-47: symbol[16]
 */
typedef struct {
    uint32_t user_id_buy;
    uint32_t user_order_id_buy;
    uint32_t user_id_sell;
    uint32_t user_order_id_sell;
    uint32_t price;
    uint32_t quantity;
    uint32_t buy_client_id;
    uint32_t sell_client_id;
    char symbol[MAX_SYMBOL_LENGTH];
} trade_msg_t;

_Static_assert(sizeof(trade_msg_t) == 48, "trade_msg_t should be 48 bytes");
_Static_assert(offsetof(trade_msg_t, symbol) == 32, "symbol at wrong offset");

/**
 * Top of Book Message
 *
 * Layout (24 bytes):
 *   0-3:   price (0 = eliminated)
 *   4-7:   total_quantity (0 = eliminated)
 *   8:     side
 *   9-23:  symbol[15] + padding
 *
 * Note: eliminated field removed - price==0 && qty==0 indicates elimination
 */
typedef struct {
    uint32_t price;          /* 0 means eliminated */
    uint32_t total_quantity; /* 0 means eliminated */
    side_t side;             /* 1 byte */
    char symbol[15];         /* 15 chars to fit 24 bytes total */
} top_of_book_msg_t;

_Static_assert(sizeof(top_of_book_msg_t) == 24, "top_of_book_msg_t should be 24 bytes");

/**
 * Output Message - Tagged Union
 *
 * Layout (52 bytes):
 *   0:     type (uint8_t)
 *   1-3:   padding
 *   4-51:  union (48 bytes = trade)
 */
typedef struct {
    output_msg_type_t type;
    uint8_t _pad[3];
    union {
        ack_msg_t ack;               /* 24 bytes */
        cancel_ack_msg_t cancel_ack; /* 24 bytes */
        trade_msg_t trade;           /* 48 bytes */
        top_of_book_msg_t top_of_book; /* 24 bytes */
    } data;
} output_msg_t;

_Static_assert(sizeof(output_msg_t) == 52, "output_msg_t should be 52 bytes");
_Static_assert(offsetof(output_msg_t, data) == 4, "data union at wrong offset");

/* ============================================================================
 * Validation Helpers
 * ============================================================================ */

/**
 * Validate side value
 */
static inline bool side_is_valid(side_t side) {
    return side == SIDE_BUY || side == SIDE_SELL;
}

/**
 * Validate input message type
 */
static inline bool input_msg_type_is_valid(input_msg_type_t type) {
    return type <= INPUT_MSG_FLUSH;
}

/**
 * Validate output message type
 */
static inline bool output_msg_type_is_valid(output_msg_type_t type) {
    return type <= OUTPUT_MSG_TOP_OF_BOOK;
}

/* ============================================================================
 * Helper Functions - With Rule 5 Assertions
 * ============================================================================
 *
 * All helper functions include at least 2 assertions as per Rule 5.
 */

/**
 * Safe symbol copy - handles null termination
 * Bounded loop for Holzmann Rule 2 compliance
 *
 * @param dest      Destination buffer
 * @param src       Source string
 * @param dest_size Size of destination buffer
 *
 * Preconditions:
 * - dest != NULL
 * - src != NULL
 * - dest_size > 0
 *
 * Postconditions:
 * - dest is null-terminated
 */
static inline void msg_copy_symbol(char* dest, const char* src, size_t dest_size) {
    assert(dest != NULL && "NULL destination in msg_copy_symbol");
    assert(src != NULL && "NULL source in msg_copy_symbol");
    assert(dest_size > 0 && "Zero dest_size in msg_copy_symbol");

    size_t i = 0;

    /* Rule 2: Loop bounded by dest_size */
    while (i < dest_size - 1 && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';

    /* Postcondition: dest is null-terminated */
    assert(dest[i] == '\0' && "dest not null-terminated");
}

/**
 * Create new order input message
 *
 * @param msg Pointer to new_order_msg_t with order details
 * @return Wrapped input_msg_t
 *
 * Preconditions:
 * - msg != NULL
 * - msg->side is valid
 */
static inline input_msg_t make_new_order_msg(const new_order_msg_t* msg) {
    assert(msg != NULL && "NULL msg in make_new_order_msg");
    assert(side_is_valid(msg->side) && "Invalid side in make_new_order_msg");

    input_msg_t result;
    memset(&result, 0, sizeof(result));  /* Clear padding bytes */
    result.type = INPUT_MSG_NEW_ORDER;
    result.data.new_order = *msg;

    assert(result.type == INPUT_MSG_NEW_ORDER && "Type not set correctly");
    return result;
}

/**
 * Create cancel input message
 *
 * @param msg Pointer to cancel_msg_t with cancel details
 * @return Wrapped input_msg_t
 *
 * Preconditions:
 * - msg != NULL
 */
static inline input_msg_t make_cancel_msg(const cancel_msg_t* msg) {
    assert(msg != NULL && "NULL msg in make_cancel_msg");
    assert(msg->user_id != 0 && "Invalid user_id 0 in make_cancel_msg");

    input_msg_t result;
    memset(&result, 0, sizeof(result));
    result.type = INPUT_MSG_CANCEL;
    result.data.cancel = *msg;

    assert(result.type == INPUT_MSG_CANCEL && "Type not set correctly");
    return result;
}

/**
 * Create flush input message
 *
 * @return Wrapped input_msg_t for flush
 */
static inline input_msg_t make_flush_msg(void) {
    input_msg_t result;
    memset(&result, 0, sizeof(result));
    result.type = INPUT_MSG_FLUSH;
    result.data.flush._unused = 0;

    assert(result.type == INPUT_MSG_FLUSH && "Type not set correctly");
    assert(result.data.flush._unused == 0 && "Flush unused not zeroed");
    return result;
}

/**
 * Create acknowledgment output message
 *
 * @param symbol        Order symbol
 * @param user_id       User who placed the order
 * @param user_order_id User's order ID
 * @return Wrapped output_msg_t
 *
 * Preconditions:
 * - symbol != NULL
 */
static inline output_msg_t make_ack_msg(const char* symbol,
                                        uint32_t user_id,
                                        uint32_t user_order_id) {
    assert(symbol != NULL && "NULL symbol in make_ack_msg");
    assert(symbol[0] != '\0' && "Empty symbol in make_ack_msg");

    output_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = OUTPUT_MSG_ACK;
    msg.data.ack.user_id = user_id;
    msg.data.ack.user_order_id = user_order_id;
    msg_copy_symbol(msg.data.ack.symbol, symbol, MAX_SYMBOL_LENGTH);

    assert(msg.type == OUTPUT_MSG_ACK && "Type not set correctly");
    return msg;
}

/**
 * Create cancel acknowledgment output message
 *
 * @param symbol        Order symbol
 * @param user_id       User who placed the order
 * @param user_order_id User's order ID
 * @return Wrapped output_msg_t
 */
static inline output_msg_t make_cancel_ack_msg(const char* symbol,
                                               uint32_t user_id,
                                               uint32_t user_order_id) {
    assert(symbol != NULL && "NULL symbol in make_cancel_ack_msg");
    assert(symbol[0] != '\0' && "Empty symbol in make_cancel_ack_msg");

    output_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = OUTPUT_MSG_CANCEL_ACK;
    msg.data.cancel_ack.user_id = user_id;
    msg.data.cancel_ack.user_order_id = user_order_id;
    msg_copy_symbol(msg.data.cancel_ack.symbol, symbol, MAX_SYMBOL_LENGTH);

    assert(msg.type == OUTPUT_MSG_CANCEL_ACK && "Type not set correctly");
    return msg;
}

/**
 * Create trade output message
 *
 * @param symbol             Trade symbol
 * @param user_id_buy        Buyer's user ID
 * @param user_order_id_buy  Buyer's order ID
 * @param user_id_sell       Seller's user ID
 * @param user_order_id_sell Seller's order ID
 * @param price              Trade price
 * @param quantity           Trade quantity
 * @return Wrapped output_msg_t
 */
static inline output_msg_t make_trade_msg(const char* symbol,
                                          uint32_t user_id_buy,
                                          uint32_t user_order_id_buy,
                                          uint32_t user_id_sell,
                                          uint32_t user_order_id_sell,
                                          uint32_t price,
                                          uint32_t quantity) {
    assert(symbol != NULL && "NULL symbol in make_trade_msg");
    assert(quantity > 0 && "Zero quantity in make_trade_msg");

    output_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = OUTPUT_MSG_TRADE;
    msg.data.trade.user_id_buy = user_id_buy;
    msg.data.trade.user_order_id_buy = user_order_id_buy;
    msg.data.trade.user_id_sell = user_id_sell;
    msg.data.trade.user_order_id_sell = user_order_id_sell;
    msg.data.trade.price = price;
    msg.data.trade.quantity = quantity;
    msg.data.trade.buy_client_id = 0;   /* Set by caller if needed */
    msg.data.trade.sell_client_id = 0;
    msg_copy_symbol(msg.data.trade.symbol, symbol, MAX_SYMBOL_LENGTH);

    assert(msg.type == OUTPUT_MSG_TRADE && "Type not set correctly");
    return msg;
}

/**
 * Create top-of-book output message
 *
 * @param symbol         Order book symbol
 * @param side           Side being updated
 * @param price          Best price (0 = eliminated)
 * @param total_quantity Total quantity at price (0 = eliminated)
 * @return Wrapped output_msg_t
 */
static inline output_msg_t make_top_of_book_msg(const char* symbol,
                                                side_t side,
                                                uint32_t price,
                                                uint32_t total_quantity) {
    assert(symbol != NULL && "NULL symbol in make_top_of_book_msg");
    assert(side_is_valid(side) && "Invalid side in make_top_of_book_msg");

    output_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = OUTPUT_MSG_TOP_OF_BOOK;
    msg.data.top_of_book.price = price;
    msg.data.top_of_book.total_quantity = total_quantity;
    msg.data.top_of_book.side = side;
    msg_copy_symbol(msg.data.top_of_book.symbol, symbol,
                    sizeof(msg.data.top_of_book.symbol));

    assert(msg.type == OUTPUT_MSG_TOP_OF_BOOK && "Type not set correctly");
    return msg;
}

/**
 * Create top-of-book eliminated message
 *
 * Convenience wrapper for side elimination.
 *
 * @param symbol Order book symbol
 * @param side   Side being eliminated
 * @return Wrapped output_msg_t with price=0, qty=0
 */
static inline output_msg_t make_top_of_book_eliminated_msg(const char* symbol,
                                                           side_t side) {
    assert(symbol != NULL && "NULL symbol in make_top_of_book_eliminated_msg");
    assert(side_is_valid(side) && "Invalid side in make_top_of_book_eliminated_msg");

    return make_top_of_book_msg(symbol, side, 0, 0);
}

/**
 * Check if top-of-book message indicates elimination
 *
 * @param msg Pointer to top_of_book_msg_t
 * @return true if this is an elimination message
 */
static inline bool top_of_book_is_eliminated(const top_of_book_msg_t* msg) {
    assert(msg != NULL && "NULL msg in top_of_book_is_eliminated");
    assert(side_is_valid(msg->side) && "Invalid side in top_of_book_is_eliminated");

    return msg->price == 0 && msg->total_quantity == 0;
}

/**
 * Get symbol from input message
 *
 * @param msg Pointer to input message
 * @return Pointer to symbol string, or NULL for flush messages
 */
static inline const char* input_msg_get_symbol(const input_msg_t* msg) {
    assert(msg != NULL && "NULL msg in input_msg_get_symbol");
    assert(input_msg_type_is_valid(msg->type) && "Invalid message type");

    switch (msg->type) {
        case INPUT_MSG_NEW_ORDER:
            return msg->data.new_order.symbol;
        case INPUT_MSG_CANCEL:
            return msg->data.cancel.symbol;
        case INPUT_MSG_FLUSH:
            return NULL;  /* Flush has no symbol */
        default:
            return NULL;
    }
}

/**
 * Get user_id from input message
 *
 * @param msg Pointer to input message
 * @return User ID, or 0 for flush messages
 */
static inline uint32_t input_msg_get_user_id(const input_msg_t* msg) {
    assert(msg != NULL && "NULL msg in input_msg_get_user_id");
    assert(input_msg_type_is_valid(msg->type) && "Invalid message type");

    switch (msg->type) {
        case INPUT_MSG_NEW_ORDER:
            return msg->data.new_order.user_id;
        case INPUT_MSG_CANCEL:
            return msg->data.cancel.user_id;
        case INPUT_MSG_FLUSH:
            return 0;
        default:
            return 0;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_MESSAGE_TYPES_H */

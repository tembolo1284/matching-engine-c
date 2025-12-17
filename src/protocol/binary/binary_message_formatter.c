#include "protocol/binary/binary_message_formatter.h"
#include <string.h>
#include <arpa/inet.h>
#include <assert.h>

/**
 * Binary Message Formatter Implementation
 *
 * Formats output messages to wire-format binary for transmission.
 * This is the high-performance path for HFT applications.
 *
 * Performance characteristics:
 * - No string formatting or memory allocation
 * - Direct memory writes with byte order conversion
 * - ~10-20ns per message format
 *
 * Rule compliance:
 * - Rule 5: All functions have >= 2 assertions
 */

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize binary message formatter
 *
 * Clears the internal buffer.
 *
 * Preconditions:
 * - formatter != NULL
 */
void binary_message_formatter_init(binary_message_formatter_t* formatter) {
    assert(formatter != NULL && "NULL formatter in binary_message_formatter_init");

    memset(&formatter->buffer, 0, sizeof(formatter->buffer));

    /* Postcondition: buffer is zeroed */
    assert(formatter->buffer.ack.magic == 0 && "Buffer not zeroed");
}

/* ============================================================================
 * Symbol Formatting Helper
 * ============================================================================ */

/**
 * Copy symbol to binary format (pad with nulls)
 *
 * Ensures the symbol field is exactly BINARY_SYMBOL_LEN bytes,
 * padding with nulls if the source is shorter.
 *
 * @param dest Destination buffer (exactly BINARY_SYMBOL_LEN bytes)
 * @param src  Source null-terminated string
 *
 * Preconditions:
 * - dest != NULL
 * - src != NULL
 */
static inline void format_symbol(char* dest, const char* src) {
    assert(dest != NULL && "NULL dest in format_symbol");
    assert(src != NULL && "NULL src in format_symbol");

    /* Copy characters up to BINARY_SYMBOL_LEN or null terminator */
    size_t i = 0;
    for (; i < BINARY_SYMBOL_LEN && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }

    /* Pad remaining with nulls */
    for (; i < BINARY_SYMBOL_LEN; i++) {
        dest[i] = '\0';
    }
}

/* ============================================================================
 * Individual Message Formatters
 * ============================================================================ */

/**
 * Format acknowledgement message
 *
 * Wire format (18 bytes):
 *   magic(1) + type(1) + symbol(8) + user_id(4) + user_order_id(4)
 *
 * Preconditions:
 * - formatter != NULL
 * - msg != NULL
 *
 * Returns: Size of formatted message
 */
static size_t format_ack(binary_message_formatter_t* formatter,
                         const ack_msg_t* msg) {
    assert(formatter != NULL && "NULL formatter in format_ack");
    assert(msg != NULL && "NULL msg in format_ack");

    binary_ack_t* bin = &formatter->buffer.ack;

    bin->magic = BINARY_MAGIC;
    bin->msg_type = BINARY_MSG_ACK;
    format_symbol(bin->symbol, msg->symbol);
    bin->user_id = htonl(msg->user_id);
    bin->user_order_id = htonl(msg->user_order_id);

    /* Postcondition: magic byte set correctly */
    assert(bin->magic == BINARY_MAGIC && "Magic byte not set");

    return sizeof(binary_ack_t);
}

/**
 * Format cancel acknowledgement message
 *
 * Wire format (18 bytes):
 *   magic(1) + type(1) + symbol(8) + user_id(4) + user_order_id(4)
 *
 * Preconditions:
 * - formatter != NULL
 * - msg != NULL
 */
static size_t format_cancel_ack(binary_message_formatter_t* formatter,
                                const cancel_ack_msg_t* msg) {
    assert(formatter != NULL && "NULL formatter in format_cancel_ack");
    assert(msg != NULL && "NULL msg in format_cancel_ack");

    binary_cancel_ack_t* bin = &formatter->buffer.cancel_ack;

    bin->magic = BINARY_MAGIC;
    bin->msg_type = BINARY_MSG_CANCEL_ACK;
    format_symbol(bin->symbol, msg->symbol);
    bin->user_id = htonl(msg->user_id);
    bin->user_order_id = htonl(msg->user_order_id);

    assert(bin->magic == BINARY_MAGIC && "Magic byte not set");

    return sizeof(binary_cancel_ack_t);
}

/**
 * Format trade message
 *
 * Wire format (34 bytes):
 *   magic(1) + type(1) + symbol(8) +
 *   user_id_buy(4) + user_order_id_buy(4) +
 *   user_id_sell(4) + user_order_id_sell(4) +
 *   price(4) + quantity(4)
 *
 * Preconditions:
 * - formatter != NULL
 * - msg != NULL
 */
static size_t format_trade(binary_message_formatter_t* formatter,
                           const trade_msg_t* msg) {
    assert(formatter != NULL && "NULL formatter in format_trade");
    assert(msg != NULL && "NULL msg in format_trade");

    binary_trade_t* bin = &formatter->buffer.trade;

    bin->magic = BINARY_MAGIC;
    bin->msg_type = BINARY_MSG_TRADE;
    format_symbol(bin->symbol, msg->symbol);
    bin->user_id_buy = htonl(msg->user_id_buy);
    bin->user_order_id_buy = htonl(msg->user_order_id_buy);
    bin->user_id_sell = htonl(msg->user_id_sell);
    bin->user_order_id_sell = htonl(msg->user_order_id_sell);
    bin->price = htonl(msg->price);
    bin->quantity = htonl(msg->quantity);

    assert(bin->magic == BINARY_MAGIC && "Magic byte not set");

    return sizeof(binary_trade_t);
}

/**
 * Format top-of-book message
 *
 * Wire format (19 bytes):
 *   magic(1) + type(1) + symbol(8) + side(1) + price(4) + quantity(4)
 *
 * For eliminated side, price and quantity are 0.
 *
 * Preconditions:
 * - formatter != NULL
 * - msg != NULL
 */
static size_t format_top_of_book(binary_message_formatter_t* formatter,
                                 const top_of_book_msg_t* msg) {
    assert(formatter != NULL && "NULL formatter in format_top_of_book");
    assert(msg != NULL && "NULL msg in format_top_of_book");

    binary_top_of_book_t* bin = &formatter->buffer.top_of_book;

    bin->magic = BINARY_MAGIC;
    bin->msg_type = BINARY_MSG_TOP_OF_BOOK;
    format_symbol(bin->symbol, msg->symbol);
    bin->side = (msg->side == SIDE_BUY) ? 'B' : 'S';

    if (top_of_book_is_eliminated(msg)) {
        bin->price = 0;
        bin->quantity = 0;
    } else {
        bin->price = htonl(msg->price);
        bin->quantity = htonl(msg->total_quantity);
    }

    assert(bin->magic == BINARY_MAGIC && "Magic byte not set");

    return sizeof(binary_top_of_book_t);
}

/* ============================================================================
 * Main Format Function
 * ============================================================================ */

/**
 * Format an output message to binary
 *
 * Returns pointer to internal buffer and sets *out_len to message size.
 * Buffer is valid until next format call on this formatter.
 *
 * @param formatter The formatter instance
 * @param msg       Output message to format
 * @param out_len   Output: size of formatted message
 * @return Pointer to formatted binary data, or NULL on error
 *
 * Preconditions:
 * - formatter != NULL
 * - msg != NULL
 * - out_len != NULL
 *
 * Postconditions on success:
 * - Returns non-NULL pointer
 * - *out_len > 0
 * - First byte of result is BINARY_MAGIC
 */
const void* binary_message_formatter_format(binary_message_formatter_t* formatter,
                                            const output_msg_t* msg,
                                            size_t* out_len) {
    assert(formatter != NULL && "NULL formatter in binary_message_formatter_format");
    assert(msg != NULL && "NULL msg in binary_message_formatter_format");
    assert(out_len != NULL && "NULL out_len in binary_message_formatter_format");

    switch (msg->type) {
        case OUTPUT_MSG_ACK:
            *out_len = format_ack(formatter, &msg->data.ack);
            break;

        case OUTPUT_MSG_CANCEL_ACK:
            *out_len = format_cancel_ack(formatter, &msg->data.cancel_ack);
            break;

        case OUTPUT_MSG_TRADE:
            *out_len = format_trade(formatter, &msg->data.trade);
            break;

        case OUTPUT_MSG_TOP_OF_BOOK:
            *out_len = format_top_of_book(formatter, &msg->data.top_of_book);
            break;

        default:
            *out_len = 0;
            return NULL;
    }

    /* Postcondition: valid formatted message */
    assert(*out_len > 0 && "Zero length output");

    const uint8_t* result = (const uint8_t*)&formatter->buffer;
    assert(result[0] == BINARY_MAGIC && "Magic byte not in output");

    return &formatter->buffer;
}

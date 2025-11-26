#include "protocol/binary/binary_message_formatter.h"
#include <string.h>
#include <arpa/inet.h>

/**
 * Initialize binary message formatter
 */
void binary_message_formatter_init(binary_message_formatter_t* formatter) {
    memset(&formatter->buffer, 0, sizeof(formatter->buffer));
}

/**
 * Helper: Copy symbol to binary format (pad with nulls)
 */
static void format_symbol(char* dest, const char* src) {
    size_t i;
    for (i = 0; i < BINARY_SYMBOL_LEN && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    /* Pad remaining with nulls */
    for (; i < BINARY_SYMBOL_LEN; i++) {
        dest[i] = '\0';
    }
}

/**
 * Format acknowledgement message
 */
static size_t format_ack(binary_message_formatter_t* formatter, const ack_msg_t* msg) {
    binary_ack_t* bin = &formatter->buffer.ack;
    
    bin->magic = BINARY_MAGIC;
    bin->msg_type = BINARY_MSG_ACK;
    format_symbol(bin->symbol, msg->symbol);
    bin->user_id = htonl(msg->user_id);
    bin->user_order_id = htonl(msg->user_order_id);
    
    return sizeof(binary_ack_t);
}

/**
 * Format cancel acknowledgement message
 */
static size_t format_cancel_ack(binary_message_formatter_t* formatter, const cancel_ack_msg_t* msg) {
    binary_cancel_ack_t* bin = &formatter->buffer.cancel_ack;
    
    bin->magic = BINARY_MAGIC;
    bin->msg_type = BINARY_MSG_CANCEL_ACK;
    format_symbol(bin->symbol, msg->symbol);
    bin->user_id = htonl(msg->user_id);
    bin->user_order_id = htonl(msg->user_order_id);
    
    return sizeof(binary_cancel_ack_t);
}

/**
 * Format trade message
 */
static size_t format_trade(binary_message_formatter_t* formatter, const trade_msg_t* msg) {
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
    
    return sizeof(binary_trade_t);
}

/**
 * Format top-of-book message
 */
static size_t format_top_of_book(binary_message_formatter_t* formatter, const top_of_book_msg_t* msg) {
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
    
    return sizeof(binary_top_of_book_t);
}

/**
 * Format an output message to binary
 */
const void* binary_message_formatter_format(binary_message_formatter_t* formatter,
                                            const output_msg_t* msg,
                                            size_t* out_len) {
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
    
    return &formatter->buffer;
}

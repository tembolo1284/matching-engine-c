#include "protocol/csv/message_formatter.h"
#include <stdio.h>
#include <string.h>

/**
 * Initialize message formatter
 */
void message_formatter_init(message_formatter_t* formatter) {
    memset(formatter->buffer, 0, MAX_OUTPUT_LINE_LENGTH);
}

/**
 * Format an output message to string
 * Returns pointer to internal buffer (valid until next format call)
 */
const char* message_formatter_format(message_formatter_t* formatter, const output_msg_t* msg) {
    switch (msg->type) {
        case OUTPUT_MSG_ACK:
            snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                    "A, %s, %u, %u",
                    msg->data.ack.symbol,
                    msg->data.ack.user_id,
                    msg->data.ack.user_order_id);
            break;
            
        case OUTPUT_MSG_CANCEL_ACK:
            snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                    "C, %s, %u, %u",
                    msg->data.cancel_ack.symbol,
                    msg->data.cancel_ack.user_id,
                    msg->data.cancel_ack.user_order_id);
            break;
            
        case OUTPUT_MSG_TRADE:
            snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                    "T, %s, %u, %u, %u, %u, %u, %u",
                    msg->data.trade.symbol,
                    msg->data.trade.user_id_buy,
                    msg->data.trade.user_order_id_buy,
                    msg->data.trade.user_id_sell,
                    msg->data.trade.user_order_id_sell,
                    msg->data.trade.price,
                    msg->data.trade.quantity);
            break;
            
        case OUTPUT_MSG_TOP_OF_BOOK:
            if (msg->data.top_of_book.price == 0) {
                /* Top of book eliminated */
                snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                        "B, %s, %s, -, -",
                        msg->data.top_of_book.symbol,
                        msg->data.top_of_book.side == SIDE_BUY ? "B" : "S");
            } else {
                snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                        "B, %s, %s, %u, %u",
                        msg->data.top_of_book.symbol,
                        msg->data.top_of_book.side == SIDE_BUY ? "B" : "S",
                        msg->data.top_of_book.price,
                        msg->data.top_of_book.total_quantity);
            }
            break;
            
        default:
            snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH, "UNKNOWN");
            break;
    }
    
    return formatter->buffer;
}

/**
 * Format acknowledgement message
 * Format: A, userId, userOrderId
 */
int format_ack(char* buffer, size_t buffer_size, const ack_msg_t* msg) {
    return snprintf(buffer, buffer_size, "A, %u, %u", 
                    msg->user_id, msg->user_order_id);
}

/**
 * Format cancel acknowledgement message
 * Format: C, userId, userOrderId
 */
int format_cancel_ack(char* buffer, size_t buffer_size, const cancel_ack_msg_t* msg) {
    return snprintf(buffer, buffer_size, "C, %u, %u", 
                    msg->user_id, msg->user_order_id);
}

/**
 * Format trade message
 * Format: T, userIdBuy, userOrderIdBuy, userIdSell, userOrderIdSell, price, quantity
 */
int format_trade(char* buffer, size_t buffer_size, const trade_msg_t* msg) {
    return snprintf(buffer, buffer_size, "T, %u, %u, %u, %u, %u, %u",
                    msg->user_id_buy,
                    msg->user_order_id_buy,
                    msg->user_id_sell,
                    msg->user_order_id_sell,
                    msg->price,
                    msg->quantity);
}

/**
 * Format top-of-book message
 * Format: B, side (B or S), price, totalQuantity
 * Eliminated: B, side (B or S), -, -
 */
int format_top_of_book(char* buffer, size_t buffer_size, const top_of_book_msg_t* msg) {
    if (top_of_book_is_eliminated(msg)) {
        return snprintf(buffer, buffer_size, "B, %c, -, -", (char)msg->side);
    } else {
        return snprintf(buffer, buffer_size, "B, %c, %u, %u",
                        (char)msg->side,
                        msg->price,
                        msg->total_quantity);
    }
}

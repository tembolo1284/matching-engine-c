#include "protocol/csv/message_formatter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/**
 * CSV Message Formatter Implementation
 *
 * Formats output messages to human-readable CSV format.
 * Used for CSV protocol clients and debugging.
 *
 * Performance notes:
 * - snprintf is slower than binary formatting (~100ns vs ~10ns)
 * - Acceptable for CSV clients which are not latency-critical
 * - Binary protocol should be used for HFT applications
 *
 * Rule compliance:
 * - Rule 5: All functions have >= 2 assertions
 * - Rule 7: All snprintf return values checked
 */

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize message formatter
 *
 * Preconditions:
 * - formatter != NULL
 */
void message_formatter_init(message_formatter_t* formatter) {
    assert(formatter != NULL && "NULL formatter in message_formatter_init");

    memset(formatter->buffer, 0, MAX_OUTPUT_LINE_LENGTH);

    /* Postcondition: buffer is zeroed */
    assert(formatter->buffer[0] == '\0' && "Buffer not zeroed");
}

/* ============================================================================
 * Main Formatting Function
 * ============================================================================ */

/**
 * Format an output message to string
 *
 * Returns pointer to internal buffer (valid until next format call).
 * Buffer is always null-terminated.
 *
 * Preconditions:
 * - formatter != NULL
 * - msg != NULL
 *
 * Postconditions:
 * - Returns non-NULL pointer to null-terminated string
 * - String length < MAX_OUTPUT_LINE_LENGTH
 */
const char* message_formatter_format(message_formatter_t* formatter,
                                      const output_msg_t* msg) {
    assert(formatter != NULL && "NULL formatter in message_formatter_format");
    assert(msg != NULL && "NULL msg in message_formatter_format");

    int written = 0;

    switch (msg->type) {
        case OUTPUT_MSG_ACK:
            written = snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                               "A, %s, %u, %u\n",
                               msg->data.ack.symbol,
                               msg->data.ack.user_id,
                               msg->data.ack.user_order_id);
            break;

        case OUTPUT_MSG_CANCEL_ACK:
            written = snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                               "C, %s, %u, %u\n",
                               msg->data.cancel_ack.symbol,
                               msg->data.cancel_ack.user_id,
                               msg->data.cancel_ack.user_order_id);
            break;

        case OUTPUT_MSG_TRADE:
            written = snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                               "T, %s, %u, %u, %u, %u, %u, %u\n",
                               msg->data.trade.symbol,
                               msg->data.trade.user_id_buy,
                               msg->data.trade.user_order_id_buy,
                               msg->data.trade.user_id_sell,
                               msg->data.trade.user_order_id_sell,
                               msg->data.trade.price,
                               msg->data.trade.quantity);
            break;

        case OUTPUT_MSG_TOP_OF_BOOK:
            if (msg->data.top_of_book.price == 0 &&
                msg->data.top_of_book.total_quantity == 0) {
                /* Top of book eliminated */
                written = snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                                   "B, %s, %c, -, -\n",
                                   msg->data.top_of_book.symbol,
                                   (char)msg->data.top_of_book.side);
            } else {
                written = snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                                   "B, %s, %c, %u, %u\n",
                                   msg->data.top_of_book.symbol,
                                   (char)msg->data.top_of_book.side,
                                   msg->data.top_of_book.price,
                                   msg->data.top_of_book.total_quantity);
            }
            break;

        default:
            written = snprintf(formatter->buffer, MAX_OUTPUT_LINE_LENGTH,
                               "UNKNOWN\n");
            break;
    }

    /* Rule 7: Check snprintf return value */
    if (written < 0) {
        /* Encoding error - return empty string */
        formatter->buffer[0] = '\0';
    } else if (written >= MAX_OUTPUT_LINE_LENGTH) {
        /* Truncation occurred - ensure null termination */
        formatter->buffer[MAX_OUTPUT_LINE_LENGTH - 1] = '\0';
    }

    /* Postcondition: buffer is null-terminated */
    assert(formatter->buffer[MAX_OUTPUT_LINE_LENGTH - 1] == '\0' ||
           strlen(formatter->buffer) < MAX_OUTPUT_LINE_LENGTH);

    return formatter->buffer;
}

/* ============================================================================
 * Individual Message Type Formatters
 * ============================================================================ */

/**
 * Format acknowledgement message
 *
 * Format: A, userId, userOrderId\n
 *
 * Preconditions:
 * - buffer != NULL
 * - buffer_size > 0
 * - msg != NULL
 *
 * Returns: Number of characters written (excluding null), or negative on error
 */
int format_ack(char* buffer, size_t buffer_size, const ack_msg_t* msg) {
    assert(buffer != NULL && "NULL buffer in format_ack");
    assert(buffer_size > 0 && "Zero buffer_size in format_ack");
    assert(msg != NULL && "NULL msg in format_ack");

    int written = snprintf(buffer, buffer_size, "A, %u, %u\n",
                           msg->user_id, msg->user_order_id);

    /* Rule 7: Check return value */
    if (written < 0 || (size_t)written >= buffer_size) {
        buffer[0] = '\0';  /* Ensure null-terminated on error */
        return -1;
    }

    return written;
}

/**
 * Format cancel acknowledgement message
 *
 * Format: C, userId, userOrderId\n
 *
 * Preconditions:
 * - buffer != NULL
 * - buffer_size > 0
 * - msg != NULL
 */
int format_cancel_ack(char* buffer, size_t buffer_size,
                      const cancel_ack_msg_t* msg) {
    assert(buffer != NULL && "NULL buffer in format_cancel_ack");
    assert(buffer_size > 0 && "Zero buffer_size in format_cancel_ack");
    assert(msg != NULL && "NULL msg in format_cancel_ack");

    int written = snprintf(buffer, buffer_size, "C, %u, %u\n",
                           msg->user_id, msg->user_order_id);

    if (written < 0 || (size_t)written >= buffer_size) {
        buffer[0] = '\0';
        return -1;
    }

    return written;
}

/**
 * Format trade message
 *
 * Format: T, userIdBuy, userOrderIdBuy, userIdSell, userOrderIdSell, price, quantity\n
 *
 * Preconditions:
 * - buffer != NULL
 * - buffer_size > 0
 * - msg != NULL
 */
int format_trade(char* buffer, size_t buffer_size, const trade_msg_t* msg) {
    assert(buffer != NULL && "NULL buffer in format_trade");
    assert(buffer_size > 0 && "Zero buffer_size in format_trade");
    assert(msg != NULL && "NULL msg in format_trade");

    int written = snprintf(buffer, buffer_size,
                           "T, %u, %u, %u, %u, %u, %u\n",
                           msg->user_id_buy,
                           msg->user_order_id_buy,
                           msg->user_id_sell,
                           msg->user_order_id_sell,
                           msg->price,
                           msg->quantity);

    if (written < 0 || (size_t)written >= buffer_size) {
        buffer[0] = '\0';
        return -1;
    }

    return written;
}

/**
 * Format top-of-book message
 *
 * Format: B, side (B or S), price, totalQuantity\n
 * Eliminated: B, side (B or S), -, -\n
 *
 * Preconditions:
 * - buffer != NULL
 * - buffer_size > 0
 * - msg != NULL
 */
int format_top_of_book(char* buffer, size_t buffer_size,
                       const top_of_book_msg_t* msg) {
    assert(buffer != NULL && "NULL buffer in format_top_of_book");
    assert(buffer_size > 0 && "Zero buffer_size in format_top_of_book");
    assert(msg != NULL && "NULL msg in format_top_of_book");

    int written;

    if (top_of_book_is_eliminated(msg)) {
        written = snprintf(buffer, buffer_size, "B, %c, -, -\n",
                           (char)msg->side);
    } else {
        written = snprintf(buffer, buffer_size, "B, %c, %u, %u\n",
                           (char)msg->side,
                           msg->price,
                           msg->total_quantity);
    }

    if (written < 0 || (size_t)written >= buffer_size) {
        buffer[0] = '\0';
        return -1;
    }

    return written;
}

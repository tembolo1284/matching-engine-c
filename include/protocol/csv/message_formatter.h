#ifndef MATCHING_ENGINE_MESSAGE_FORMATTER_H
#define MATCHING_ENGINE_MESSAGE_FORMATTER_H

#include "protocol/message_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MessageFormatter - Format output messages to CSV
 * 
 * Output format:
 * - Acknowledgement: A, userId, userOrderId
 * - Cancel ack:      C, userId, userOrderId
 * - Trade:           T, userIdBuy, userOrderIdBuy, userIdSell, userOrderIdSell, price, quantity
 * - Top of book:     B, side (B or S), price, totalQuantity
 * - TOB eliminated:  B, side (B or S), -, -
 * 
 * Design decisions:
 * - Produces comma-space separated values for readability
 * - Handles special case of eliminated side (-, -)
 * - Uses fixed-size output buffer (no dynamic allocation)
 * - Thread-safe (no global state)
 */

#define MAX_OUTPUT_LINE_LENGTH 512

/**
 * Formatter state (for buffer reuse if needed)
 */
typedef struct {
    char buffer[MAX_OUTPUT_LINE_LENGTH];
} message_formatter_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize message formatter
 */
void message_formatter_init(message_formatter_t* formatter);

/**
 * Format an output message to string
 * Returns pointer to internal buffer (valid until next format call)
 * Buffer is null-terminated
 */
const char* message_formatter_format(message_formatter_t* formatter, const output_msg_t* msg);

/* ============================================================================
 * Helper Functions (Internal)
 * ============================================================================ */

/**
 * Format specific message types (write to buffer, return length)
 */
int format_ack(char* buffer, size_t buffer_size, const ack_msg_t* msg);
int format_cancel_ack(char* buffer, size_t buffer_size, const cancel_ack_msg_t* msg);
int format_trade(char* buffer, size_t buffer_size, const trade_msg_t* msg);
int format_top_of_book(char* buffer, size_t buffer_size, const top_of_book_msg_t* msg);

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_MESSAGE_FORMATTER_H */

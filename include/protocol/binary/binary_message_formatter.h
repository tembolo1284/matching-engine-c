#ifndef BINARY_MESSAGE_FORMATTER_H
#define BINARY_MESSAGE_FORMATTER_H

#include "protocol/message_types.h"
#include "protocol/binary/binary_protocol.h"
#include <stddef.h>

/**
 * Binary message formatter
 * Converts output_msg_t to binary format
 */
typedef struct {
    union {
        binary_ack_t ack;
        binary_cancel_ack_t cancel_ack;
        binary_trade_t trade;
        binary_top_of_book_t top_of_book;
    } buffer;
} binary_message_formatter_t;

/**
 * Initialize binary message formatter
 */
void binary_message_formatter_init(binary_message_formatter_t* formatter);

/**
 * Format an output message to binary
 * Returns pointer to internal buffer and sets *out_len to message size
 * Buffer is valid until next format call
 */
const void* binary_message_formatter_format(binary_message_formatter_t* formatter,
                                            const output_msg_t* msg,
                                            size_t* out_len);

#endif /* BINARY_MESSAGE_FORMATTER_H */

/**
 * codec.h - Encoding/decoding layer for matching engine client
 *
 * Provides unified interface for CSV and Binary protocols with:
 *   - Auto-detection of server response format
 *   - Encoding of outgoing messages
 *   - Decoding of incoming messages
 *
 * Reuses:
 *   - protocol/message_types.h (message structs)
 *   - protocol/csv/message_parser.h (CSV parsing)
 *   - protocol/csv/message_formatter.h (CSV formatting)
 *   - protocol/binary/binary_protocol.h (binary wire format)
 *   - protocol/binary/binary_message_parser.h (binary parsing)
 *   - protocol/binary/binary_message_formatter.h (binary formatting)
 */

#ifndef CLIENT_CODEC_H
#define CLIENT_CODEC_H

#include "client/client_config.h"
#include "protocol/message_types.h"
#include "protocol/csv/message_parser.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_protocol.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/binary/binary_message_formatter.h"

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Constants
 * ============================================================ */

#define CODEC_MAX_MESSAGE_SIZE  1024
#define CODEC_MAX_CSV_LINE      512

/* ============================================================
 * Codec Handle
 * ============================================================ */

/**
 * Codec state - wraps CSV and binary parsers/formatters
 */
typedef struct {
    /* Configured encoding for sending */
    encoding_type_t         send_encoding;
    
    /* Detected encoding from server responses */
    encoding_type_t         detected_encoding;
    bool                    encoding_detected;
    
    /* CSV parser/formatter */
    message_parser_t        csv_parser;
    message_formatter_t     csv_formatter;
    
    /* Binary parser/formatter */
    binary_message_parser_t     binary_parser;
    binary_message_formatter_t  binary_formatter;
    
    /* Output buffer for encoded messages */
    char                    encode_buffer[CODEC_MAX_MESSAGE_SIZE];
    size_t                  encode_len;
    
    /* Statistics */
    uint64_t                messages_encoded;
    uint64_t                messages_decoded;
    uint64_t                decode_errors;
    
} codec_t;

/* ============================================================
 * Codec API
 * ============================================================ */

/**
 * Initialize codec
 * 
 * @param c             Codec handle
 * @param send_encoding Encoding to use for outgoing messages
 *                      (ENCODING_AUTO defaults to BINARY)
 */
void codec_init(codec_t* c, encoding_type_t send_encoding);

/**
 * Reset codec state (clear detected encoding, stats)
 */
void codec_reset(codec_t* c);

/* ============================================================
 * Encoding (Client -> Server)
 * ============================================================ */

/**
 * Encode a new order message
 * 
 * @param c             Codec handle
 * @param user_id       User ID
 * @param symbol        Trading symbol
 * @param price         Price (0 for market order)
 * @param quantity      Order quantity
 * @param side          SIDE_BUY or SIDE_SELL
 * @param order_id      Client order ID
 * @param out_data      Output: pointer to encoded data
 * @param out_len       Output: encoded data length
 * @return              true on success
 */
bool codec_encode_new_order(codec_t* c,
                            uint32_t user_id,
                            const char* symbol,
                            uint32_t price,
                            uint32_t quantity,
                            side_t side,
                            uint32_t order_id,
                            const void** out_data,
                            size_t* out_len);

/**
 * Encode a cancel message
 * 
 * @param c             Codec handle
 * @param user_id       User ID
 * @param order_id      Order ID to cancel
 * @param out_data      Output: pointer to encoded data
 * @param out_len       Output: encoded data length
 * @return              true on success
 */
bool codec_encode_cancel(codec_t* c,
                         uint32_t user_id,
                         uint32_t order_id,
                         const void** out_data,
                         size_t* out_len);

/**
 * Encode a flush message
 * 
 * @param c             Codec handle
 * @param out_data      Output: pointer to encoded data
 * @param out_len       Output: encoded data length
 * @return              true on success
 */
bool codec_encode_flush(codec_t* c,
                        const void** out_data,
                        size_t* out_len);

/* ============================================================
 * Decoding (Server -> Client)
 * ============================================================ */

/**
 * Detect encoding of received data
 * 
 * @param data          Received data
 * @param len           Data length
 * @return              Detected encoding type
 */
encoding_type_t codec_detect_encoding(const void* data, size_t len);

/**
 * Decode a server response message
 * 
 * Auto-detects encoding based on first byte.
 * Updates codec's detected_encoding field.
 * 
 * @param c             Codec handle
 * @param data          Received data
 * @param len           Data length
 * @param msg           Output: decoded message
 * @return              true on success
 */
bool codec_decode_response(codec_t* c,
                           const void* data,
                           size_t len,
                           output_msg_t* msg);

/* ============================================================
 * Message Formatting (for display)
 * ============================================================ */

/**
 * Format output message to human-readable string
 * 
 * @param c             Codec handle
 * @param msg           Message to format
 * @return              Pointer to formatted string (valid until next call)
 */
const char* codec_format_output(codec_t* c, const output_msg_t* msg);

/**
 * Format input message to human-readable string
 * 
 * @param msg           Message to format
 * @param buffer        Output buffer
 * @param buffer_size   Buffer size
 * @return              Number of characters written
 */
int codec_format_input(const input_msg_t* msg, char* buffer, size_t buffer_size);

/* ============================================================
 * Utilities
 * ============================================================ */

/**
 * Get current send encoding
 */
encoding_type_t codec_get_send_encoding(const codec_t* c);

/**
 * Get detected server encoding
 */
encoding_type_t codec_get_detected_encoding(const codec_t* c);

/**
 * Check if server encoding has been detected
 */
bool codec_is_encoding_detected(const codec_t* c);

/**
 * Print codec statistics
 */
void codec_print_stats(const codec_t* c);

/**
 * Get output message type as string
 */
const char* output_msg_type_str(output_msg_type_t type);

/**
 * Get side as character
 */
static inline char side_char(side_t side) {
    return (char)side;  /* SIDE_BUY = 'B', SIDE_SELL = 'S' */
}

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_CODEC_H */

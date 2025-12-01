/**
 * codec.c - Encoding/decoding layer implementation
 */

#include "client/codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>  /* For htonl/ntohl */

/* ============================================================
 * Initialization
 * ============================================================ */

void codec_init(codec_t* c, encoding_type_t send_encoding) {
    memset(c, 0, sizeof(*c));

    /* Default to binary if auto */
    c->send_encoding = (send_encoding == ENCODING_AUTO) ? ENCODING_BINARY : send_encoding;
    c->detected_encoding = ENCODING_AUTO;
    c->encoding_detected = false;

    /* Initialize parsers/formatters */
    message_parser_init(&c->csv_parser);
    message_formatter_init(&c->csv_formatter);
    binary_message_parser_init(&c->binary_parser);
    binary_message_formatter_init(&c->binary_formatter);
}

void codec_reset(codec_t* c) {
    c->detected_encoding = ENCODING_AUTO;
    c->encoding_detected = false;
    c->messages_encoded = 0;
    c->messages_decoded = 0;
    c->decode_errors = 0;
}

/* ============================================================
 * Binary Encoding Helpers
 * ============================================================ */

static size_t encode_binary_new_order(char* buffer,
                                      uint32_t user_id,
                                      const char* symbol,
                                      uint32_t price,
                                      uint32_t quantity,
                                      side_t side,
                                      uint32_t order_id) {
    binary_new_order_t* msg = (binary_new_order_t*)buffer;

    msg->magic = BINARY_MAGIC;
    msg->msg_type = BINARY_MSG_NEW_ORDER;
    msg->user_id = htonl(user_id);
    msg->price = htonl(price);
    msg->quantity = htonl(quantity);
    msg->side = side;
    msg->user_order_id = htonl(order_id);

    /* Copy symbol with null padding - use memcpy to avoid strncpy truncation warning */
    memset(msg->symbol, 0, BINARY_SYMBOL_LEN);
    size_t sym_len = strlen(symbol);
    if (sym_len > BINARY_SYMBOL_LEN) {
        sym_len = BINARY_SYMBOL_LEN;
    }
    memcpy(msg->symbol, symbol, sym_len);

    return sizeof(binary_new_order_t);
}

static size_t encode_binary_cancel(char* buffer,
                                   uint32_t user_id,
                                   uint32_t order_id) {
    binary_cancel_t* msg = (binary_cancel_t*)buffer;

    msg->magic = BINARY_MAGIC;
    msg->msg_type = BINARY_MSG_CANCEL;
    msg->user_id = htonl(user_id);
    msg->user_order_id = htonl(order_id);

    return sizeof(binary_cancel_t);
}

static size_t encode_binary_flush(char* buffer) {
    binary_flush_t* msg = (binary_flush_t*)buffer;

    msg->magic = BINARY_MAGIC;
    msg->msg_type = BINARY_MSG_FLUSH;

    return sizeof(binary_flush_t);
}

/* ============================================================
 * CSV Encoding Helpers
 * ============================================================ */

static size_t encode_csv_new_order(char* buffer, size_t buffer_size,
                                   uint32_t user_id,
                                   const char* symbol,
                                   uint32_t price,
                                   uint32_t quantity,
                                   side_t side,
                                   uint32_t order_id) {
    int len = snprintf(buffer, buffer_size,
                       "N, %u, %s, %u, %u, %c, %u\n",
                       user_id, symbol, price, quantity,
                       (char)side, order_id);
    return (len > 0 && (size_t)len < buffer_size) ? (size_t)len : 0;
}

static size_t encode_csv_cancel(char* buffer, size_t buffer_size,
                                uint32_t user_id,
                                uint32_t order_id) {
    int len = snprintf(buffer, buffer_size,
                       "C, %u, %u\n",
                       user_id, order_id);
    return (len > 0 && (size_t)len < buffer_size) ? (size_t)len : 0;
}

static size_t encode_csv_flush(char* buffer, size_t buffer_size) {
    int len = snprintf(buffer, buffer_size, "F\n");
    return (len > 0 && (size_t)len < buffer_size) ? (size_t)len : 0;
}

/* ============================================================
 * Encoding API
 * ============================================================ */

bool codec_encode_new_order(codec_t* c,
                            uint32_t user_id,
                            const char* symbol,
                            uint32_t price,
                            uint32_t quantity,
                            side_t side,
                            uint32_t order_id,
                            const void** out_data,
                            size_t* out_len) {
    if (c->send_encoding == ENCODING_BINARY) {
        c->encode_len = encode_binary_new_order(c->encode_buffer,
                                                user_id, symbol, price,
                                                quantity, side, order_id);
    } else {
        c->encode_len = encode_csv_new_order(c->encode_buffer, sizeof(c->encode_buffer),
                                             user_id, symbol, price,
                                             quantity, side, order_id);
    }

    if (c->encode_len == 0) {
        return false;
    }

    *out_data = c->encode_buffer;
    *out_len = c->encode_len;
    c->messages_encoded++;
    return true;
}

bool codec_encode_cancel(codec_t* c,
                         uint32_t user_id,
                         uint32_t order_id,
                         const void** out_data,
                         size_t* out_len) {
    if (c->send_encoding == ENCODING_BINARY) {
        c->encode_len = encode_binary_cancel(c->encode_buffer, user_id, order_id);
    } else {
        c->encode_len = encode_csv_cancel(c->encode_buffer, sizeof(c->encode_buffer),
                                          user_id, order_id);
    }

    if (c->encode_len == 0) {
        return false;
    }

    *out_data = c->encode_buffer;
    *out_len = c->encode_len;
    c->messages_encoded++;
    return true;
}

bool codec_encode_flush(codec_t* c,
                        const void** out_data,
                        size_t* out_len) {
    if (c->send_encoding == ENCODING_BINARY) {
        c->encode_len = encode_binary_flush(c->encode_buffer);
    } else {
        c->encode_len = encode_csv_flush(c->encode_buffer, sizeof(c->encode_buffer));
    }

    if (c->encode_len == 0) {
        return false;
    }

    *out_data = c->encode_buffer;
    *out_len = c->encode_len;
    c->messages_encoded++;
    return true;
}

/* ============================================================
 * Decoding
 * ============================================================ */

encoding_type_t codec_detect_encoding(const void* data, size_t len) {
    if (len == 0) {
        return ENCODING_AUTO;
    }

    /* Check for binary magic byte */
    if (is_binary_message((const char*)data, len)) {
        return ENCODING_BINARY;
    }

    return ENCODING_CSV;
}

/**
 * Decode binary output message
 */
static bool decode_binary_output(const void* data, size_t len, output_msg_t* msg) {
    if (len < 2) {
        return false;
    }

    const uint8_t* bytes = (const uint8_t*)data;

    if (bytes[0] != BINARY_MAGIC) {
        return false;
    }

    uint8_t msg_type = bytes[1];

    switch (msg_type) {
        case BINARY_MSG_ACK: {
            if (len < sizeof(binary_ack_t)) return false;
            const binary_ack_t* ack = (const binary_ack_t*)data;

            msg->type = OUTPUT_MSG_ACK;
            msg->data.ack.user_id = ntohl(ack->user_id);
            msg->data.ack.user_order_id = ntohl(ack->user_order_id);
            memset(msg->data.ack.symbol, 0, MAX_SYMBOL_LENGTH);
            memcpy(msg->data.ack.symbol, ack->symbol, BINARY_SYMBOL_LEN);
            return true;
        }

        case BINARY_MSG_CANCEL_ACK: {
            if (len < sizeof(binary_cancel_ack_t)) return false;
            const binary_cancel_ack_t* cack = (const binary_cancel_ack_t*)data;

            msg->type = OUTPUT_MSG_CANCEL_ACK;
            msg->data.cancel_ack.user_id = ntohl(cack->user_id);
            msg->data.cancel_ack.user_order_id = ntohl(cack->user_order_id);
            memset(msg->data.cancel_ack.symbol, 0, MAX_SYMBOL_LENGTH);
            memcpy(msg->data.cancel_ack.symbol, cack->symbol, BINARY_SYMBOL_LEN);
            return true;
        }

        case BINARY_MSG_TRADE: {
            if (len < sizeof(binary_trade_t)) return false;
            const binary_trade_t* trade = (const binary_trade_t*)data;

            msg->type = OUTPUT_MSG_TRADE;
            msg->data.trade.user_id_buy = ntohl(trade->user_id_buy);
            msg->data.trade.user_order_id_buy = ntohl(trade->user_order_id_buy);
            msg->data.trade.user_id_sell = ntohl(trade->user_id_sell);
            msg->data.trade.user_order_id_sell = ntohl(trade->user_order_id_sell);
            msg->data.trade.price = ntohl(trade->price);
            msg->data.trade.quantity = ntohl(trade->quantity);
            msg->data.trade.buy_client_id = 0;
            msg->data.trade.sell_client_id = 0;
            memset(msg->data.trade.symbol, 0, MAX_SYMBOL_LENGTH);
            memcpy(msg->data.trade.symbol, trade->symbol, BINARY_SYMBOL_LEN);
            return true;
        }

        case BINARY_MSG_TOP_OF_BOOK: {
            if (len < sizeof(binary_top_of_book_t)) return false;
            const binary_top_of_book_t* tob = (const binary_top_of_book_t*)data;

            msg->type = OUTPUT_MSG_TOP_OF_BOOK;
            msg->data.top_of_book.price = ntohl(tob->price);
            msg->data.top_of_book.total_quantity = ntohl(tob->quantity);
            msg->data.top_of_book.side = tob->side;
            memset(msg->data.top_of_book.symbol, 0, sizeof(msg->data.top_of_book.symbol));
            memcpy(msg->data.top_of_book.symbol, tob->symbol, BINARY_SYMBOL_LEN);
            return true;
        }

        default:
            return false;
    }
}

/**
 * Decode CSV output message
 *
 * Output formats:
 *   A, symbol, userId, orderId
 *   C, symbol, userId, orderId
 *   T, symbol, buyUser, buyOrd, sellUser, sellOrd, price, qty
 *   B, symbol, side, price, qty   (or B, symbol, side, -, -)
 */
static bool decode_csv_output(const char* data, size_t len, output_msg_t* msg) {
    /* Make a mutable copy for tokenization */
    char line[512];
    if (len >= sizeof(line)) {
        return false;
    }
    memcpy(line, data, len);
    line[len] = '\0';

    /* Trim newline */
    char* newline = strchr(line, '\n');
    if (newline) *newline = '\0';
    newline = strchr(line, '\r');
    if (newline) *newline = '\0';

    /* Get message type */
    char* saveptr;
    char* token = strtok_r(line, ",", &saveptr);
    if (!token) return false;

    /* Trim whitespace */
    while (*token == ' ') token++;

    char msg_type = token[0];

    switch (msg_type) {
        case 'A': {
            /* A, symbol, userId, orderId */
            msg->type = OUTPUT_MSG_ACK;

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            while (*token == ' ') token++;
            strncpy(msg->data.ack.symbol, token, MAX_SYMBOL_LENGTH - 1);
            msg->data.ack.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.ack.user_id = (uint32_t)atoi(token);

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.ack.user_order_id = (uint32_t)atoi(token);

            return true;
        }

        case 'C': {
            /* C, symbol, userId, orderId */
            msg->type = OUTPUT_MSG_CANCEL_ACK;

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            while (*token == ' ') token++;
            strncpy(msg->data.cancel_ack.symbol, token, MAX_SYMBOL_LENGTH - 1);
            msg->data.cancel_ack.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.cancel_ack.user_id = (uint32_t)atoi(token);

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.cancel_ack.user_order_id = (uint32_t)atoi(token);

            return true;
        }

        case 'T': {
            /* T, symbol, buyUser, buyOrd, sellUser, sellOrd, price, qty */
            msg->type = OUTPUT_MSG_TRADE;

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            while (*token == ' ') token++;
            strncpy(msg->data.trade.symbol, token, MAX_SYMBOL_LENGTH - 1);
            msg->data.trade.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.trade.user_id_buy = (uint32_t)atoi(token);

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.trade.user_order_id_buy = (uint32_t)atoi(token);

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.trade.user_id_sell = (uint32_t)atoi(token);

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.trade.user_order_id_sell = (uint32_t)atoi(token);

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.trade.price = (uint32_t)atoi(token);

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            msg->data.trade.quantity = (uint32_t)atoi(token);

            msg->data.trade.buy_client_id = 0;
            msg->data.trade.sell_client_id = 0;

            return true;
        }

        case 'B': {
            /* B, symbol, side, price, qty  or  B, symbol, side, -, - */
            msg->type = OUTPUT_MSG_TOP_OF_BOOK;

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            while (*token == ' ') token++;
            strncpy(msg->data.top_of_book.symbol, token, sizeof(msg->data.top_of_book.symbol) - 1);
            msg->data.top_of_book.symbol[sizeof(msg->data.top_of_book.symbol) - 1] = '\0';

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            while (*token == ' ') token++;
            msg->data.top_of_book.side = (side_t)token[0];

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            while (*token == ' ') token++;
            if (token[0] == '-') {
                msg->data.top_of_book.price = 0;
            } else {
                msg->data.top_of_book.price = (uint32_t)atoi(token);
            }

            token = strtok_r(NULL, ",", &saveptr);
            if (!token) return false;
            while (*token == ' ') token++;
            if (token[0] == '-') {
                msg->data.top_of_book.total_quantity = 0;
            } else {
                msg->data.top_of_book.total_quantity = (uint32_t)atoi(token);
            }

            return true;
        }

        default:
            return false;
    }
}

bool codec_decode_response(codec_t* c,
                           const void* data,
                           size_t len,
                           output_msg_t* msg) {
    if (len == 0) {
        c->decode_errors++;
        return false;
    }

    /* Auto-detect encoding */
    encoding_type_t encoding = codec_detect_encoding(data, len);

    /* Update detected encoding */
    if (!c->encoding_detected) {
        c->detected_encoding = encoding;
        c->encoding_detected = true;
    }

    bool success;
    if (encoding == ENCODING_BINARY) {
        success = decode_binary_output(data, len, msg);
    } else {
        success = decode_csv_output((const char*)data, len, msg);
    }

    if (success) {
        c->messages_decoded++;
    } else {
        c->decode_errors++;
    }

    return success;
}

/* ============================================================
 * Formatting
 * ============================================================ */

const char* codec_format_output(codec_t* c, const output_msg_t* msg) {
    return message_formatter_format(&c->csv_formatter, msg);
}

int codec_format_input(const input_msg_t* msg, char* buffer, size_t buffer_size) {
    switch (msg->type) {
        case INPUT_MSG_NEW_ORDER:
            return snprintf(buffer, buffer_size,
                           "N, %u, %s, %u, %u, %c, %u",
                           msg->data.new_order.user_id,
                           msg->data.new_order.symbol,
                           msg->data.new_order.price,
                           msg->data.new_order.quantity,
                           (char)msg->data.new_order.side,
                           msg->data.new_order.user_order_id);

        case INPUT_MSG_CANCEL:
            return snprintf(buffer, buffer_size,
                           "C, %u, %u",
                           msg->data.cancel.user_id,
                           msg->data.cancel.user_order_id);

        case INPUT_MSG_FLUSH:
            return snprintf(buffer, buffer_size, "F");

        default:
            return snprintf(buffer, buffer_size, "UNKNOWN");
    }
}

/* ============================================================
 * Utilities
 * ============================================================ */

encoding_type_t codec_get_send_encoding(const codec_t* c) {
    return c->send_encoding;
}

encoding_type_t codec_get_detected_encoding(const codec_t* c) {
    return c->detected_encoding;
}

bool codec_is_encoding_detected(const codec_t* c) {
    return c->encoding_detected;
}

void codec_print_stats(const codec_t* c) {
    printf("Codec Statistics:\n");
    printf("  Send encoding:     %s\n", encoding_type_str(c->send_encoding));
    printf("  Detected encoding: %s\n",
           c->encoding_detected ? encoding_type_str(c->detected_encoding) : "not yet");
    printf("  Messages encoded:  %lu\n", (unsigned long)c->messages_encoded);
    printf("  Messages decoded:  %lu\n", (unsigned long)c->messages_decoded);
    printf("  Decode errors:     %lu\n", (unsigned long)c->decode_errors);
}

const char* output_msg_type_str(output_msg_type_t type) {
    switch (type) {
        case OUTPUT_MSG_ACK:         return "ACK";
        case OUTPUT_MSG_CANCEL_ACK:  return "CANCEL_ACK";
        case OUTPUT_MSG_TRADE:       return "TRADE";
        case OUTPUT_MSG_TOP_OF_BOOK: return "TOP_OF_BOOK";
        default:                     return "UNKNOWN";
    }
}

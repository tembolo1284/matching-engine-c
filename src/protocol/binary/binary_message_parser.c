#include "protocol/binary/binary_message_parser.h"
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>

/**
 * Initialize binary message parser
 */
void binary_message_parser_init(binary_message_parser_t* parser) {
    parser->placeholder = 0;
}

/**
 * Helper: Copy and null-terminate symbol from binary format
 */
static void copy_symbol(char* dest, const char* src) {
    size_t i;
    for (i = 0; i < BINARY_SYMBOL_LEN && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

/**
 * Parse binary new order message
 */
static bool parse_new_order(const binary_new_order_t* bin_msg, input_msg_t* msg) {
    msg->type = INPUT_MSG_NEW_ORDER;
    
    msg->data.new_order.user_id = ntohl(bin_msg->user_id);
    copy_symbol(msg->data.new_order.symbol, bin_msg->symbol);
    msg->data.new_order.price = ntohl(bin_msg->price);
    msg->data.new_order.quantity = ntohl(bin_msg->quantity);
    msg->data.new_order.side = (bin_msg->side == 'B') ? SIDE_BUY : SIDE_SELL;
    msg->data.new_order.user_order_id = ntohl(bin_msg->user_order_id);
    
    return true;
}

/**
 * Parse binary cancel message
 */
static bool parse_cancel(const binary_cancel_t* bin_msg, input_msg_t* msg) {
    msg->type = INPUT_MSG_CANCEL;
    
    msg->data.cancel.user_id = ntohl(bin_msg->user_id);
    msg->data.cancel.user_order_id = ntohl(bin_msg->user_order_id);
    
    return true;
}

/**
 * Parse binary flush message
 */
static bool parse_flush(input_msg_t* msg) {
    msg->type = INPUT_MSG_FLUSH;
    return true;
}

/**
 * Parse binary message into input_msg_t
 */
bool binary_message_parser_parse(binary_message_parser_t* parser,
                                  const void* data,
                                  size_t len,
                                  input_msg_t* msg) {
    (void)parser;  /* Currently unused */
    
    if (len < 2) {
        fprintf(stderr, "Binary message too short: %zu bytes\n", len);
        return false;
    }
    
    const uint8_t* bytes = (const uint8_t*)data;
    
    /* Check magic byte */
    if (bytes[0] != BINARY_MAGIC) {
        fprintf(stderr, "Invalid binary magic byte: 0x%02X\n", bytes[0]);
        return false;
    }
    
    uint8_t msg_type = bytes[1];
    
    switch (msg_type) {
        case BINARY_MSG_NEW_ORDER:
            if (len < sizeof(binary_new_order_t)) {
                fprintf(stderr, "Binary new order too short: %zu bytes\n", len);
                return false;
            }
            return parse_new_order((const binary_new_order_t*)data, msg);
            
        case BINARY_MSG_CANCEL:
            if (len < sizeof(binary_cancel_t)) {
                fprintf(stderr, "Binary cancel too short: %zu bytes\n", len);
                return false;
            }
            return parse_cancel((const binary_cancel_t*)data, msg);
            
        case BINARY_MSG_FLUSH:
            if (len < sizeof(binary_flush_t)) {
                fprintf(stderr, "Binary flush too short: %zu bytes\n", len);
                return false;
            }
            return parse_flush(msg);
            
        default:
            fprintf(stderr, "Unknown binary message type: 0x%02X\n", msg_type);
            return false;
    }
}

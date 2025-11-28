#include "protocol/csv/message_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/**
 * Initialize message parser
 */
void message_parser_init(message_parser_t* parser) {
    memset(parser, 0, sizeof(message_parser_t));
}

/**
 * Parse a single line of input
 * Returns true if valid message parsed, false for comments/blank lines/invalid
 */
bool message_parser_parse(message_parser_t* parser, const char* line, input_msg_t* msg) {
    // Copy line to internal buffer
    strncpy(parser->line_buffer, line, MAX_LINE_LENGTH - 1);
    parser->line_buffer[MAX_LINE_LENGTH - 1] = '\0';
    
    // Trim whitespace
    trim_whitespace(parser->line_buffer);
    
    // Skip empty lines and comments
    if (parser->line_buffer[0] == '\0' || parser->line_buffer[0] == '#') {
        return false;
    }
    
    // Split by comma
    parser->num_tokens = split_string(parser->line_buffer, ',', parser->tokens, MAX_TOKENS);
    
    if (parser->num_tokens == 0) {
        return false;
    }
    
    // Trim each token
    for (int i = 0; i < parser->num_tokens; i++) {
        trim_whitespace(parser->tokens[i]);
    }
    
    // Parse based on first token (message type)
    char msg_type = parser->tokens[0][0];
    
    switch (msg_type) {
        case 'N':
            return parse_new_order(parser, msg);
        case 'C':
            return parse_cancel(parser, msg);
        case 'F':
            return parse_flush(parser, msg);
        default:
            return false;
    }
}

/**
 * Parse new order message
 * Format: N, user(int), symbol(string), price(int), qty(int), side(char B or S), userOrderId(int)
 */
bool parse_new_order(message_parser_t* parser, input_msg_t* msg) {
    if (parser->num_tokens != 7) {
        return false;
    }
    
    new_order_msg_t order;
    
    // Parse fields
    order.user_id = parse_uint32(parser->tokens[1]);
    
    // Symbol
    size_t sym_len = strlen(parser->tokens[2]);
    if (sym_len >= MAX_SYMBOL_LENGTH) {
        sym_len = MAX_SYMBOL_LENGTH - 1;
    }
    memcpy(order.symbol, parser->tokens[2], sym_len);
    order.symbol[sym_len] = '\0'; 
    order.price = parse_uint32(parser->tokens[3]);
    order.quantity = parse_uint32(parser->tokens[4]);
    
    // Side
    if (!parse_side(parser->tokens[5], &order.side)) {
        return false;
    }
    
    order.user_order_id = parse_uint32(parser->tokens[6]);
    
    // Validate
    if (order.quantity == 0) {
        return false;
    }
    
    // Create message
    *msg = make_new_order_msg(&order);
    return true;
}

/**
 * Parse cancel message
 * Format: C, user(int), userOrderId(int)
 */
bool parse_cancel(message_parser_t* parser, input_msg_t* msg) {
    if (parser->num_tokens != 3) {
        return false;
    }
    
    cancel_msg_t cancel;
    cancel.user_id = parse_uint32(parser->tokens[1]);
    cancel.user_order_id = parse_uint32(parser->tokens[2]);
    
    *msg = make_cancel_msg(&cancel);
    return true;
}

/**
 * Parse flush message
 * Format: F
 */
bool parse_flush(message_parser_t* parser, input_msg_t* msg) {
    if (parser->num_tokens != 1) {
        return false;
    }
    
    *msg = make_flush_msg();
    return true;
}

/**
 * Trim whitespace from string (modifies in place)
 */
void trim_whitespace(char* str) {
    if (str == NULL || str[0] == '\0') {
        return;
    }
    
    // Trim leading whitespace
    char* start = str;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    
    // Trim trailing whitespace
    char* end = str + strlen(str) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        end--;
    }
    
    // Write new null terminator
    *(end + 1) = '\0';
    
    // Move string to beginning if needed
    if (start != str) {
        memmove(str, start, end - start + 2);
    }
}

/**
 * Split string by delimiter, returns number of tokens
 */
int split_string(const char* str, char delimiter, char tokens[][MAX_TOKEN_LENGTH], int max_tokens) {
    int token_count = 0;
    const char* start = str;
    const char* end = str;
    
    while (*end != '\0' && token_count < max_tokens) {
        if (*end == delimiter) {
            // Found delimiter - copy token
            size_t len = end - start;
            if (len >= MAX_TOKEN_LENGTH) {
                len = MAX_TOKEN_LENGTH - 1;
            }
            strncpy(tokens[token_count], start, len);
            tokens[token_count][len] = '\0';
            token_count++;
            
            // Move to next token
            start = end + 1;
        }
        end++;
    }
    
    // Copy last token
    if (*start != '\0' && token_count < max_tokens) {
        size_t len = end - start;
        if (len >= MAX_TOKEN_LENGTH) {
            len = MAX_TOKEN_LENGTH - 1;
        }
        strncpy(tokens[token_count], start, len);
        tokens[token_count][len] = '\0';
        token_count++;
    }
    
    return token_count;
}

/**
 * Parse unsigned 32-bit integer, returns 0 on failure
 */
uint32_t parse_uint32(const char* str) {
    char* endptr;
    unsigned long val = strtoul(str, &endptr, 10);
    
    // Check for conversion errors
    if (endptr == str || *endptr != '\0') {
        return 0;
    }
    
    // Check for overflow
    if (val > UINT32_MAX) {
        return 0;
    }
    
    return (uint32_t)val;
}

/**
 * Parse side character ('B' or 'S')
 */
bool parse_side(const char* str, side_t* side) {
    if (str == NULL || str[0] == '\0') {
        return false;
    }
    
    if (str[0] == 'B') {
        *side = SIDE_BUY;
        return true;
    } else if (str[0] == 'S') {
        *side = SIDE_SELL;
        return true;
    }
    
    return false;
}

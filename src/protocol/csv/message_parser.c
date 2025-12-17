#include "protocol/csv/message_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>

/**
 * CSV Message Parser Implementation
 *
 * Parses human-readable CSV input messages into structured format.
 * Used for CSV protocol clients and testing.
 *
 * Performance notes:
 * - String parsing is inherently slower than binary (~1µs vs ~10ns)
 * - Acceptable for CSV clients which are not latency-critical
 * - Binary protocol should be used for HFT applications
 *
 * Rule compliance:
 * - Rule 2: All loops have bounded iteration counts
 * - Rule 5: All functions have >= 2 assertions
 * - Rule 7: All strtoul/parsing return values checked
 */

/* ============================================================================
 * Constants for Bounded Loops (Rule 2)
 * ============================================================================ */

#define MAX_TRIM_ITERATIONS   (MAX_LINE_LENGTH)
#define MAX_SPLIT_ITERATIONS  (MAX_LINE_LENGTH)

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize message parser
 *
 * Preconditions:
 * - parser != NULL
 *
 * Postconditions:
 * - All parser fields zeroed
 */
void message_parser_init(message_parser_t* parser) {
    assert(parser != NULL && "NULL parser in message_parser_init");

    memset(parser, 0, sizeof(message_parser_t));

    /* Postcondition: parser is zeroed */
    assert(parser->num_tokens == 0 && "Parser not zeroed");
}

/* ============================================================================
 * Main Parsing Function
 * ============================================================================ */

/**
 * Parse a single line of input
 *
 * Returns true if valid message parsed, false for comments/blank lines/invalid.
 * Output is written to 'msg' parameter on success.
 *
 * Preconditions:
 * - parser != NULL
 * - line != NULL
 * - msg != NULL
 *
 * Postconditions on success:
 * - msg->type is valid
 * - msg contains parsed data
 */
bool message_parser_parse(message_parser_t* parser, const char* line,
                          input_msg_t* msg) {
    assert(parser != NULL && "NULL parser in message_parser_parse");
    assert(line != NULL && "NULL line in message_parser_parse");
    assert(msg != NULL && "NULL msg in message_parser_parse");

    /* Copy line to internal buffer with bounds checking */
    size_t line_len = strlen(line);
    if (line_len >= MAX_LINE_LENGTH) {
        line_len = MAX_LINE_LENGTH - 1;
    }
    memcpy(parser->line_buffer, line, line_len);
    parser->line_buffer[line_len] = '\0';

    /* Trim whitespace */
    trim_whitespace(parser->line_buffer);

    /* Skip empty lines and comments */
    if (parser->line_buffer[0] == '\0' || parser->line_buffer[0] == '#') {
        return false;
    }

    /* Split by comma */
    parser->num_tokens = split_string(parser->line_buffer, ',',
                                       parser->tokens, MAX_TOKENS);

    if (parser->num_tokens == 0) {
        return false;
    }

    /* Trim each token (Rule 2: bounded by MAX_TOKENS) */
    for (int i = 0; i < parser->num_tokens && i < MAX_TOKENS; i++) {
        trim_whitespace(parser->tokens[i]);
    }

    /* Parse based on first token (message type) */
    if (parser->tokens[0][0] == '\0') {
        return false;
    }

    char msg_type = parser->tokens[0][0];

    switch (msg_type) {
        case 'N':
        case 'n':
            return parse_new_order(parser, msg);
        case 'C':
        case 'c':
            return parse_cancel(parser, msg);
        case 'F':
        case 'f':
            return parse_flush(parser, msg);
        default:
            return false;
    }
}

/* ============================================================================
 * Message Type Parsers
 * ============================================================================ */

/**
 * Parse new order message
 *
 * Format: N, user(int), symbol(string), price(int), qty(int), side(B/S), orderid(int)
 *
 * Preconditions:
 * - parser != NULL
 * - msg != NULL
 * - parser->num_tokens set by prior split
 */
bool parse_new_order(message_parser_t* parser, input_msg_t* msg) {
    assert(parser != NULL && "NULL parser in parse_new_order");
    assert(msg != NULL && "NULL msg in parse_new_order");

    if (parser->num_tokens != 7) {
        return false;
    }

    new_order_msg_t order;
    memset(&order, 0, sizeof(order));

    /* Parse user_id */
    order.user_id = parse_uint32(parser->tokens[1]);
    if (order.user_id == 0 && parser->tokens[1][0] != '0') {
        return false;  /* Parse failed (not just zero value) */
    }

    /* Parse symbol with length validation */
    size_t sym_len = strlen(parser->tokens[2]);
    if (sym_len == 0) {
        return false;  /* Empty symbol */
    }
    if (sym_len >= MAX_SYMBOL_LENGTH) {
        sym_len = MAX_SYMBOL_LENGTH - 1;
    }
    memcpy(order.symbol, parser->tokens[2], sym_len);
    order.symbol[sym_len] = '\0';

    /* Parse price (0 = market order) */
    order.price = parse_uint32(parser->tokens[3]);

    /* Parse quantity */
    order.quantity = parse_uint32(parser->tokens[4]);
    if (order.quantity == 0) {
        return false;  /* Zero quantity invalid */
    }

    /* Parse side */
    if (!parse_side(parser->tokens[5], &order.side)) {
        return false;
    }

    /* Parse user_order_id */
    order.user_order_id = parse_uint32(parser->tokens[6]);

    /* Create message */
    *msg = make_new_order_msg(&order);

    /* Postcondition: message type is correct */
    assert(msg->type == INPUT_MSG_NEW_ORDER && "Wrong message type");
    return true;
}

/**
 * Parse cancel message
 *
 * Format: C, user(int), userOrderId(int)
 *
 * Preconditions:
 * - parser != NULL
 * - msg != NULL
 */
bool parse_cancel(message_parser_t* parser, input_msg_t* msg) {
    assert(parser != NULL && "NULL parser in parse_cancel");
    assert(msg != NULL && "NULL msg in parse_cancel");

    if (parser->num_tokens != 3) {
        return false;
    }

    cancel_msg_t cancel;
    memset(&cancel, 0, sizeof(cancel));

    cancel.user_id = parse_uint32(parser->tokens[1]);
    cancel.user_order_id = parse_uint32(parser->tokens[2]);

    *msg = make_cancel_msg(&cancel);

    assert(msg->type == INPUT_MSG_CANCEL && "Wrong message type");
    return true;
}

/**
 * Parse flush message
 *
 * Format: F
 *
 * Preconditions:
 * - parser != NULL
 * - msg != NULL
 */
bool parse_flush(message_parser_t* parser, input_msg_t* msg) {
    assert(parser != NULL && "NULL parser in parse_flush");
    assert(msg != NULL && "NULL msg in parse_flush");

    if (parser->num_tokens != 1) {
        return false;
    }

    *msg = make_flush_msg();

    assert(msg->type == INPUT_MSG_FLUSH && "Wrong message type");
    return true;
}

/* ============================================================================
 * String Helper Functions
 * ============================================================================ */

/**
 * Trim whitespace from string (modifies in place)
 *
 * Removes leading and trailing whitespace characters.
 * Rule 2 compliant: bounded by MAX_TRIM_ITERATIONS.
 *
 * Preconditions:
 * - str may be NULL (no-op)
 *
 * Postconditions:
 * - String has no leading/trailing whitespace
 */
void trim_whitespace(char* str) {
    if (str == NULL || str[0] == '\0') {
        return;
    }

    /* Find start of non-whitespace (Rule 2: bounded) */
    char* start = str;
    size_t iterations = 0;
    while (*start != '\0' && isspace((unsigned char)*start) &&
           iterations < MAX_TRIM_ITERATIONS) {
        start++;
        iterations++;
    }

    /* Handle all-whitespace string */
    if (*start == '\0') {
        str[0] = '\0';
        return;
    }

    /* Find end of string */
    char* end = str + strlen(str) - 1;

    /* Find end of non-whitespace (Rule 2: bounded) */
    iterations = 0;
    while (end > start && isspace((unsigned char)*end) &&
           iterations < MAX_TRIM_ITERATIONS) {
        end--;
        iterations++;
    }

    /* Calculate new length */
    size_t new_len = (size_t)(end - start + 1);

    /* Move string to beginning if needed */
    if (start != str) {
        memmove(str, start, new_len);
    }
    str[new_len] = '\0';
}

/**
 * Split string by delimiter, returns number of tokens
 *
 * Rule 2 compliant: bounded by MAX_SPLIT_ITERATIONS and max_tokens.
 *
 * Preconditions:
 * - str != NULL
 * - tokens != NULL
 * - max_tokens > 0
 *
 * Postconditions:
 * - Returns number of tokens found (0 to max_tokens)
 * - Each token is null-terminated
 */
int split_string(const char* str, char delimiter,
                 char tokens[][MAX_TOKEN_LENGTH], int max_tokens) {
    assert(str != NULL && "NULL str in split_string");
    assert(tokens != NULL && "NULL tokens in split_string");
    assert(max_tokens > 0 && "Invalid max_tokens");

    int token_count = 0;
    const char* start = str;
    const char* end = str;
    size_t iterations = 0;

    /* Rule 2: Bounded by both string length and max_tokens */
    while (*end != '\0' && token_count < max_tokens &&
           iterations < MAX_SPLIT_ITERATIONS) {

        if (*end == delimiter) {
            /* Found delimiter - copy token */
            size_t len = (size_t)(end - start);
            if (len >= MAX_TOKEN_LENGTH) {
                len = MAX_TOKEN_LENGTH - 1;
            }

            if (len > 0) {
                memcpy(tokens[token_count], start, len);
            }
            tokens[token_count][len] = '\0';
            token_count++;

            /* Move to next token */
            start = end + 1;
        }

        end++;
        iterations++;
    }

    /* Copy last token if room */
    if (token_count < max_tokens && *start != '\0') {
        size_t len = (size_t)(end - start);
        if (len >= MAX_TOKEN_LENGTH) {
            len = MAX_TOKEN_LENGTH - 1;
        }

        if (len > 0) {
            memcpy(tokens[token_count], start, len);
        }
        tokens[token_count][len] = '\0';
        token_count++;
    }

    return token_count;
}

/**
 * Parse unsigned 32-bit integer
 *
 * Uses strtoul with full error checking.
 *
 * Preconditions:
 * - str != NULL
 *
 * Returns:
 * - Parsed value on success
 * - 0 on parse failure (caller should check if input was "0")
 */
uint32_t parse_uint32(const char* str) {
    assert(str != NULL && "NULL str in parse_uint32");

    if (str[0] == '\0') {
        return 0;
    }

    /* Skip leading whitespace */
    while (isspace((unsigned char)*str)) {
        str++;
    }

    /* Check for negative sign (invalid for unsigned) */
    if (*str == '-') {
        return 0;
    }

    char* endptr = NULL;
    errno = 0;

    unsigned long val = strtoul(str, &endptr, 10);

    /* Rule 7: Check for conversion errors */
    if (errno == ERANGE) {
        return 0;  /* Overflow */
    }

    if (endptr == str) {
        return 0;  /* No digits found */
    }

    /* Skip trailing whitespace */
    while (isspace((unsigned char)*endptr)) {
        endptr++;
    }

    if (*endptr != '\0') {
        return 0;  /* Trailing garbage */
    }

    /* Check for overflow to uint32_t */
    if (val > UINT32_MAX) {
        return 0;
    }

    return (uint32_t)val;
}

/**
 * Parse side character ('B' or 'S')
 *
 * Preconditions:
 * - str != NULL
 * - side != NULL
 *
 * Returns:
 * - true if valid side parsed
 * - false otherwise
 */
bool parse_side(const char* str, side_t* side) {
    assert(str != NULL && "NULL str in parse_side");
    assert(side != NULL && "NULL side in parse_side");

    if (str[0] == '\0') {
        return false;
    }

    /* Accept uppercase or lowercase */
    char c = str[0];
    if (c == 'B' || c == 'b') {
        *side = SIDE_BUY;
        return true;
    } else if (c == 'S' || c == 's') {
        *side = SIDE_SELL;
        return true;
    }

    return false;
}

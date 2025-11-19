#ifndef MATCHING_ENGINE_MESSAGE_PARSER_H
#define MATCHING_ENGINE_MESSAGE_PARSER_H

#include "message_types.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MessageParser - Parse CSV input messages
 * 
 * Input format:
 * - New order: N, user(int), symbol(string), price(int), qty(int), side(char B or S), userOrderId(int)
 * - Cancel:    C, user(int), userOrderId(int)
 * - Flush:     F
 * 
 * Design decisions:
 * - Returns bool (true = success, false = invalid/comment/blank)
 * - Output parameter for parsed message
 * - Handles whitespace trimming
 * - Validates message format
 * - No dynamic allocation (uses fixed buffers)
 */

#define MAX_LINE_LENGTH 1024
#define MAX_TOKENS 16
#define MAX_TOKEN_LENGTH 128

/**
 * Parser state (if needed for stateful parsing)
 */
typedef struct {
    char line_buffer[MAX_LINE_LENGTH];
    char tokens[MAX_TOKENS][MAX_TOKEN_LENGTH];
    int num_tokens;
} message_parser_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize message parser
 */
void message_parser_init(message_parser_t* parser);

/**
 * Parse a single line of input
 * Returns true if valid message parsed, false for comments/blank lines/invalid
 * Output is written to 'msg' parameter
 */
bool message_parser_parse(message_parser_t* parser, const char* line, input_msg_t* msg);

/* ============================================================================
 * Helper Functions (Internal)
 * ============================================================================ */

/**
 * Trim whitespace from string (modifies in place)
 */
void trim_whitespace(char* str);

/**
 * Split string by delimiter, returns number of tokens
 */
int split_string(const char* str, char delimiter, char tokens[][MAX_TOKEN_LENGTH], int max_tokens);

/**
 * Parse unsigned 32-bit integer, returns 0 on failure
 */
uint32_t parse_uint32(const char* str);

/**
 * Parse side character ('B' or 'S')
 * Returns true if valid, false otherwise
 */
bool parse_side(const char* str, side_t* side);

/**
 * Parse specific message types
 */
bool parse_new_order(message_parser_t* parser, input_msg_t* msg);
bool parse_cancel(message_parser_t* parser, input_msg_t* msg);
bool parse_flush(message_parser_t* parser, input_msg_t* msg);

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_MESSAGE_PARSER_H */

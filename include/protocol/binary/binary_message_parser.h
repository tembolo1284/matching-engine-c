#ifndef BINARY_MESSAGE_PARSER_H
#define BINARY_MESSAGE_PARSER_H

#include "protocol/message_types.h"
#include "protocol/binary/binary_protocol.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Binary message parser
 */
typedef struct {
    /* Currently stateless, but included for API consistency */
    int placeholder;
} binary_message_parser_t;

/**
 * Initialize binary message parser
 */
void binary_message_parser_init(binary_message_parser_t* parser);

/**
 * Parse binary message into input_msg_t
 * Returns true on success, false on parse error
 */
bool binary_message_parser_parse(binary_message_parser_t* parser,
                                  const void* data,
                                  size_t len,
                                  input_msg_t* msg);

#endif /* BINARY_MESSAGE_PARSER_H */

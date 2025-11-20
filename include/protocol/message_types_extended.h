#ifndef MESSAGE_TYPES_EXTENDED_H
#define MESSAGE_TYPES_EXTENDED_H

#include "protocol/message_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Extended Message Types for TCP Multi-Client Support
 * 
 * Wraps existing message types (input_msg_t, output_msg_t) with client
 * routing information. This allows the processor and output router to
 * know which client sent/should receive each message.
 */

/**
 * Input message envelope
 * 
 * Wraps input messages with client identification. Created by network
 * layer (tcp_listener) and consumed by processor.
 */
typedef struct {
    input_msg_t msg;           // The actual order/cancel/flush message
    uint32_t client_id;        // Which client sent this (1-based)
    uint64_t sequence;         // Sequence number (for debugging/ordering)
} input_msg_envelope_t;

/**
 * Output message envelope
 * 
 * Wraps output messages with routing information. Created by processor
 * and consumed by output router, which distributes to appropriate client
 * queues.
 */
typedef struct {
    output_msg_t msg;          // The actual ack/trade/TOB message
    uint32_t client_id;        // Which client should receive this (1-based)
    uint64_t sequence          // (always the client that sent the order)
} output_msg_envelope_t;

/**
 * Helper: Create input envelope from parsed message
 */
static inline input_msg_envelope_t
create_input_envelope(const input_msg_t* msg,
                      uint32_t client_id,
                      uint64_t sequence) {
    input_msg_envelope_t envelope;
    envelope.msg = *msg;
    envelope.client_id = client_id;
    envelope.sequence = sequence;
    return envelope;
}

/**
 * Helper: Create output envelope
 */
static inline output_msg_envelope_t
create_output_envelope(const output_msg_t* msg,
                       uint32_t client_id,
                       uint64_t sequence) {
    output_msg_envelope_t envelope;
    envelope.msg = *msg;
    envelope.client_id = client_id;
    envelope.sequence = sequence;
    return envelope;
}

#ifdef __cplusplus
}
#endif

#endif // MESSAGE_TYPES_EXTENDED_H

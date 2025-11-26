#ifndef MESSAGE_TYPES_EXTENDED_H
#define MESSAGE_TYPES_EXTENDED_H

#include "protocol/message_types.h"
#include <stdint.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Extended Message Types for TCP Multi-Client Support
 * 
 * Wraps existing message types (input_msg_t, output_msg_t) with client
 * routing information. This allows the processor and output router to
 * know which client sent/should receive each message.
 * 
 * Cache Optimization:
 * - output_msg_envelope_t is exactly 64 bytes (one cache line)
 * - input_msg_envelope_t padded to 56 bytes for efficient queue packing
 */

/**
 * Input message envelope
 * 
 * Layout (56 bytes):
 *   0-39:  input_msg_t (40 bytes)
 *   40-43: client_id
 *   44-47: padding (for uint64_t alignment)
 *   48-55: sequence
 * 
 * Note: Could pad to 64 bytes for cache alignment, but 56 bytes
 * allows better queue density. Profile to determine best tradeoff.
 */
typedef struct {
    input_msg_t msg;           /* 40 bytes - The actual order/cancel/flush */
    uint32_t client_id;        /* 4 bytes - Which client sent this (1-based) */
    uint32_t _pad;             /* 4 bytes - Explicit padding for alignment */
    uint64_t sequence;         /* 8 bytes - Sequence number */
} input_msg_envelope_t;

_Static_assert(sizeof(input_msg_envelope_t) == 56, 
               "input_msg_envelope_t should be 56 bytes");

/**
 * Output message envelope
 * 
 * Layout (64 bytes - exactly one cache line!):
 *   0-51:  output_msg_t (52 bytes)
 *   52-55: client_id
 *   56-63: sequence
 * 
 * This is ideal - each envelope fits exactly in one cache line,
 * preventing false sharing when multiple threads access the queue.
 */
#if defined(__GNUC__) || defined(__clang__)
typedef struct __attribute__((aligned(64))) {
#else
typedef struct {
#endif
    output_msg_t msg;          /* 52 bytes - The actual ack/trade/TOB */
    uint32_t client_id;        /* 4 bytes - Target client (1-based) */
    uint64_t sequence;         /* 8 bytes - Sequence number */
} output_msg_envelope_t;

_Static_assert(sizeof(output_msg_envelope_t) == 64, 
               "output_msg_envelope_t should be 64 bytes (one cache line)");
#if defined(__GNUC__) || defined(__clang__)
_Static_assert(alignof(output_msg_envelope_t) == 64,
               "output_msg_envelope_t should be cache-line aligned");
#endif

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
    envelope._pad = 0;
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

/**
 * Broadcast client ID - indicates message should go to all clients
 * Used for trades and top-of-book updates
 */
#define CLIENT_ID_BROADCAST 0

/**
 * Check if message should be broadcast to all clients
 */
static inline bool envelope_is_broadcast(const output_msg_envelope_t* env) {
    return env->client_id == CLIENT_ID_BROADCAST;
}

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_TYPES_EXTENDED_H */

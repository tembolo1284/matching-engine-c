#ifndef MESSAGE_TYPES_EXTENDED_H
#define MESSAGE_TYPES_EXTENDED_H

#include "protocol/message_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Extended Message Types for Multi-Client Support
 * 
 * Wraps existing message types (input_msg_t, output_msg_t) with client
 * routing information. This allows the processor and output router to
 * know which client sent/should receive each message.
 * 
 * Cache Optimization:
 * - output_msg_envelope_t is exactly 64 bytes (one cache line)
 * - input_msg_envelope_t padded to 64 bytes for cache alignment
 */

/* ============================================================================
 * Client ID Ranges (matching Zig's config.zig)
 * ============================================================================
 * - Broadcast:   0
 * - TCP clients: 1 to 0x7FFFFFFF  
 * - UDP clients: 0x80000001 to 0xFFFFFFFF
 */

#define CLIENT_ID_BROADCAST     0
#define CLIENT_ID_TCP_BASE      0
#define CLIENT_ID_UDP_BASE      0x80000000

static inline bool client_id_is_udp(uint32_t client_id) {
    return client_id > CLIENT_ID_UDP_BASE;
}

static inline bool client_id_is_tcp(uint32_t client_id) {
    return client_id > 0 && client_id <= CLIENT_ID_UDP_BASE;
}

/* ============================================================================
 * UDP Client Address
 * ============================================================================ */

/**
 * UDP Client Address - compact representation for hash table
 * Matches Zig's UdpClientAddr structure
 */
typedef struct {
    uint32_t addr;      /* IPv4 address in network byte order */
    uint16_t port;      /* Port in network byte order */
    uint16_t _pad;      /* Alignment padding */
} udp_client_addr_t;

_Static_assert(sizeof(udp_client_addr_t) == 8, "udp_client_addr_t must be 8 bytes");

/**
 * Check if two UDP addresses are equal
 */
static inline bool udp_client_addr_equal(const udp_client_addr_t* a, 
                                          const udp_client_addr_t* b) {
    return a->addr == b->addr && a->port == b->port;
}

/**
 * Protocol type detected for client
 */
typedef uint8_t client_protocol_t;
#define CLIENT_PROTOCOL_UNKNOWN  0
#define CLIENT_PROTOCOL_BINARY   1
#define CLIENT_PROTOCOL_CSV      2

/* ============================================================================
 * Input Envelope (64 bytes, cache-aligned)
 * ============================================================================ */

/**
 * Input message envelope
 * 
 * Layout (64 bytes):
 *   0-39:  input_msg_t (40 bytes)
 *   40-43: client_id
 *   44-51: client_addr (for UDP response routing)
 *   52-55: padding
 *   56-63: sequence
 */
typedef struct {
    input_msg_t msg;                /* 40 bytes - The actual order/cancel/flush */
    uint32_t client_id;             /* 4 bytes - Which client sent this */
    udp_client_addr_t client_addr;  /* 8 bytes - UDP address for responses */
    uint32_t _pad;                  /* 4 bytes - Padding */
    uint64_t sequence;              /* 8 bytes - Sequence number */
} input_msg_envelope_t;

_Static_assert(sizeof(input_msg_envelope_t) == 64, 
               "input_msg_envelope_t should be 64 bytes");

/* ============================================================================
 * Output Envelope (64 bytes, cache-aligned)
 * ============================================================================ */

/**
 * Output message envelope
 * 
 * Layout (64 bytes - exactly one cache line):
 *   0-51:  output_msg_t (52 bytes)
 *   52-55: client_id
 *   56-63: sequence
 */
#if defined(__GNUC__) || defined(__clang__)
typedef struct __attribute__((aligned(64))) {
#else
typedef struct {
#endif
    output_msg_t msg;          /* 52 bytes - The actual ack/trade/TOB */
    uint32_t client_id;        /* 4 bytes - Target client (0 = broadcast) */
    uint64_t sequence;         /* 8 bytes - Sequence number */
} output_msg_envelope_t;

_Static_assert(sizeof(output_msg_envelope_t) == 64, 
               "output_msg_envelope_t should be 64 bytes (one cache line)");

#if defined(__GNUC__) || defined(__clang__)
_Static_assert(alignof(output_msg_envelope_t) == 64,
               "output_msg_envelope_t should be cache-line aligned");
#endif

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Create input envelope from parsed message (UDP client)
 */
static inline input_msg_envelope_t
create_input_envelope_udp(const input_msg_t* msg,
                          uint32_t client_id,
                          const udp_client_addr_t* client_addr,
                          uint64_t sequence) {
    input_msg_envelope_t envelope;
    envelope.msg = *msg;
    envelope.client_id = client_id;
    envelope.client_addr = *client_addr;
    envelope._pad = 0;
    envelope.sequence = sequence;
    return envelope;
}

/**
 * Create input envelope (backward compatible - no client addr)
 */
static inline input_msg_envelope_t
create_input_envelope(const input_msg_t* msg,
                      uint32_t client_id,
                      uint64_t sequence) {
    input_msg_envelope_t envelope;
    envelope.msg = *msg;
    envelope.client_id = client_id;
    envelope.client_addr.addr = 0;
    envelope.client_addr.port = 0;
    envelope.client_addr._pad = 0;
    envelope._pad = 0;
    envelope.sequence = sequence;
    return envelope;
}

/**
 * Create output envelope
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
 * Check if message should be broadcast to all clients
 */
static inline bool envelope_is_broadcast(const output_msg_envelope_t* env) {
    return env->client_id == CLIENT_ID_BROADCAST;
}

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_TYPES_EXTENDED_H */

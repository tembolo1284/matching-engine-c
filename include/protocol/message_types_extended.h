#ifndef MESSAGE_TYPES_EXTENDED_H
#define MESSAGE_TYPES_EXTENDED_H

#include "protocol/message_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <string.h>
#include <assert.h>
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
 * - input_msg_envelope_t is exactly 64 bytes (one cache line)
 * - Both are cache-line aligned for optimal DMA and memory access
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
#define CLIENT_ID_INVALID       UINT32_MAX

/**
 * Check if client ID represents a UDP client
 */
static inline bool client_id_is_udp(uint32_t client_id) {
    return client_id > CLIENT_ID_UDP_BASE;
}

/**
 * Check if client ID represents a TCP client
 */
static inline bool client_id_is_tcp(uint32_t client_id) {
    return client_id > 0 && client_id <= CLIENT_ID_UDP_BASE;
}

/**
 * Check if client ID is valid (non-zero, non-max)
 */
static inline bool client_id_is_valid(uint32_t client_id) {
    return client_id != CLIENT_ID_BROADCAST && client_id != CLIENT_ID_INVALID;
}

/* ============================================================================
 * UDP Client Address
 * ============================================================================ */

/**
 * UDP Client Address - compact representation for hash table
 * Matches Zig's UdpClientAddr structure
 *
 * Layout (8 bytes):
 *   0-3: addr (IPv4 in network byte order)
 *   4-5: port (in network byte order)
 *   6-7: padding
 */
typedef struct {
    uint32_t addr;      /* IPv4 address in network byte order */
    uint16_t port;      /* Port in network byte order */
    uint16_t _pad;      /* Alignment padding */
} udp_client_addr_t;

_Static_assert(sizeof(udp_client_addr_t) == 8, "udp_client_addr_t must be 8 bytes");
_Static_assert(offsetof(udp_client_addr_t, port) == 4, "port at wrong offset");

/**
 * Check if two UDP addresses are equal
 *
 * @param a First address
 * @param b Second address
 * @return true if addresses match
 */
static inline bool udp_client_addr_equal(const udp_client_addr_t* a,
                                          const udp_client_addr_t* b) {
    assert(a != NULL && "NULL address a");
    assert(b != NULL && "NULL address b");

    return a->addr == b->addr && a->port == b->port;
}

/**
 * Check if UDP address is empty/unset
 */
static inline bool udp_client_addr_is_empty(const udp_client_addr_t* addr) {
    assert(addr != NULL && "NULL address");

    return addr->addr == 0 && addr->port == 0;
}

/**
 * Create UDP client address from sockaddr_in
 */
static inline udp_client_addr_t udp_client_addr_from_sockaddr(
    const struct sockaddr_in* sa) {

    assert(sa != NULL && "NULL sockaddr");
    assert(sa->sin_family == AF_INET && "Not IPv4 address");

    udp_client_addr_t addr;
    addr.addr = sa->sin_addr.s_addr;
    addr.port = sa->sin_port;
    addr._pad = 0;

    return addr;
}

/**
 * Protocol type detected for client
 */
typedef uint8_t client_protocol_t;
#define CLIENT_PROTOCOL_UNKNOWN  0
#define CLIENT_PROTOCOL_BINARY   1
#define CLIENT_PROTOCOL_CSV      2

/**
 * Validate client protocol value
 */
static inline bool client_protocol_is_valid(client_protocol_t proto) {
    return proto <= CLIENT_PROTOCOL_CSV;
}

/* ============================================================================
 * Input Envelope (64 bytes, cache-aligned)
 * ============================================================================ */

/**
 * Input message envelope - wraps input_msg_t with routing info
 *
 * Layout (64 bytes):
 *   0-39:  input_msg_t (40 bytes)
 *   40-43: client_id (4 bytes)
 *   44-51: client_addr (8 bytes, for UDP response routing)
 *   52-55: padding (4 bytes)
 *   56-63: sequence (8 bytes)
 *
 * Cache optimization:
 * - Exactly one cache line (64 bytes)
 * - Hot fields (msg, client_id) at start
 * - Sequence at end for atomic updates
 */
typedef struct {
    input_msg_t msg;                /* 40 bytes - The actual order/cancel/flush */
    uint32_t client_id;             /* 4 bytes - Which client sent this */
    udp_client_addr_t client_addr;  /* 8 bytes - UDP address for responses */
    uint32_t _pad;                  /* 4 bytes - Padding */
    uint64_t sequence;              /* 8 bytes - Sequence number */
} input_msg_envelope_t;

_Static_assert(sizeof(input_msg_envelope_t) == 64,
               "input_msg_envelope_t must be 64 bytes");
_Static_assert(offsetof(input_msg_envelope_t, client_id) == 40,
               "client_id at wrong offset");
_Static_assert(offsetof(input_msg_envelope_t, sequence) == 56,
               "sequence at wrong offset");

/* ============================================================================
 * Output Envelope (64 bytes, cache-aligned)
 * ============================================================================ */

/**
 * Output message envelope - wraps output_msg_t with routing info
 *
 * Layout (64 bytes - exactly one cache line):
 *   0-51:  output_msg_t (52 bytes)
 *   52-55: client_id (4 bytes)
 *   56-63: sequence (8 bytes)
 *
 * Cache optimization:
 * - Exactly one cache line for DMA efficiency
 * - Cache-line aligned for optimal memory access
 */
#if defined(__GNUC__) || defined(__clang__)
typedef struct __attribute__((aligned(64))) {
#else
typedef struct alignas(64) {
#endif
    output_msg_t msg;          /* 52 bytes - The actual ack/trade/TOB */
    uint32_t client_id;        /* 4 bytes - Target client (0 = broadcast) */
    uint64_t sequence;         /* 8 bytes - Sequence number */
} output_msg_envelope_t;

_Static_assert(sizeof(output_msg_envelope_t) == 64,
               "output_msg_envelope_t must be 64 bytes (one cache line)");
_Static_assert(offsetof(output_msg_envelope_t, client_id) == 52,
               "client_id at wrong offset");
_Static_assert(offsetof(output_msg_envelope_t, sequence) == 56,
               "sequence at wrong offset");

#if defined(__GNUC__) || defined(__clang__)
_Static_assert(alignof(output_msg_envelope_t) == 64,
               "output_msg_envelope_t must be cache-line aligned");
#endif

/* ============================================================================
 * Helper Functions - With Rule 5 Assertions
 * ============================================================================ */

/**
 * Create input envelope from parsed message (UDP client)
 *
 * @param msg         Pointer to parsed input message
 * @param client_id   Client identifier
 * @param client_addr UDP address for response routing
 * @param sequence    Message sequence number
 * @return Populated envelope
 *
 * Preconditions:
 * - msg != NULL
 * - client_addr != NULL
 */
static inline input_msg_envelope_t
create_input_envelope_udp(const input_msg_t* msg,
                          uint32_t client_id,
                          const udp_client_addr_t* client_addr,
                          uint64_t sequence) {
    assert(msg != NULL && "NULL msg in create_input_envelope_udp");
    assert(client_addr != NULL && "NULL client_addr");
    assert(input_msg_type_is_valid(msg->type) && "Invalid message type");

    input_msg_envelope_t envelope;
    memset(&envelope, 0, sizeof(envelope));  /* Clear padding */

    envelope.msg = *msg;
    envelope.client_id = client_id;
    envelope.client_addr = *client_addr;
    envelope._pad = 0;
    envelope.sequence = sequence;

    assert(envelope.msg.type == msg->type && "Message type not copied");
    return envelope;
}

/**
 * Create input envelope (backward compatible - no client addr)
 *
 * @param msg       Pointer to parsed input message
 * @param client_id Client identifier
 * @param sequence  Message sequence number
 * @return Populated envelope with zeroed client_addr
 *
 * Preconditions:
 * - msg != NULL
 */
static inline input_msg_envelope_t
create_input_envelope(const input_msg_t* msg,
                      uint32_t client_id,
                      uint64_t sequence) {
    assert(msg != NULL && "NULL msg in create_input_envelope");
    assert(input_msg_type_is_valid(msg->type) && "Invalid message type");

    input_msg_envelope_t envelope;
    memset(&envelope, 0, sizeof(envelope));

    envelope.msg = *msg;
    envelope.client_id = client_id;
    envelope.client_addr.addr = 0;
    envelope.client_addr.port = 0;
    envelope.client_addr._pad = 0;
    envelope._pad = 0;
    envelope.sequence = sequence;

    assert(envelope.msg.type == msg->type && "Message type not copied");
    return envelope;
}

/**
 * Create output envelope
 *
 * @param msg       Pointer to output message
 * @param client_id Target client (0 = broadcast)
 * @param sequence  Message sequence number
 * @return Populated envelope
 *
 * Preconditions:
 * - msg != NULL
 */
static inline output_msg_envelope_t
create_output_envelope(const output_msg_t* msg,
                       uint32_t client_id,
                       uint64_t sequence) {
    assert(msg != NULL && "NULL msg in create_output_envelope");
    assert(output_msg_type_is_valid(msg->type) && "Invalid message type");

    output_msg_envelope_t envelope;
    memset(&envelope, 0, sizeof(envelope));

    envelope.msg = *msg;
    envelope.client_id = client_id;
    envelope.sequence = sequence;

    assert(envelope.msg.type == msg->type && "Message type not copied");
    return envelope;
}

/**
 * Check if message should be broadcast to all clients
 *
 * @param env Pointer to output envelope
 * @return true if this is a broadcast message
 */
static inline bool envelope_is_broadcast(const output_msg_envelope_t* env) {
    assert(env != NULL && "NULL envelope in envelope_is_broadcast");
    assert(output_msg_type_is_valid(env->msg.type) && "Invalid message type");

    return env->client_id == CLIENT_ID_BROADCAST;
}

/**
 * Check if envelope is for a specific client
 *
 * @param env       Pointer to output envelope
 * @param client_id Client to check
 * @return true if envelope targets this client
 */
static inline bool envelope_is_for_client(const output_msg_envelope_t* env,
                                          uint32_t client_id) {
    assert(env != NULL && "NULL envelope");
    assert(client_id_is_valid(client_id) && "Invalid client_id");

    return env->client_id == client_id || env->client_id == CLIENT_ID_BROADCAST;
}

/**
 * Get message type name for debugging
 *
 * @param type Input message type
 * @return String name of type
 */
static inline const char* input_msg_type_name(input_msg_type_t type) {
    static const char* const names[] = {
        "NEW_ORDER",
        "CANCEL",
        "FLUSH",
        "UNKNOWN"
    };

    if (type <= INPUT_MSG_FLUSH) {
        return names[type];
    }
    return names[3];
}

/**
 * Get output message type name for debugging
 *
 * @param type Output message type
 * @return String name of type
 */
static inline const char* output_msg_type_name(output_msg_type_t type) {
    static const char* const names[] = {
        "ACK",
        "CANCEL_ACK",
        "TRADE",
        "TOP_OF_BOOK",
        "UNKNOWN"
    };

    if (type <= OUTPUT_MSG_TOP_OF_BOOK) {
        return names[type];
    }
    return names[4];
}

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_TYPES_EXTENDED_H */

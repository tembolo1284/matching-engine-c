#ifndef BINARY_PROTOCOL_H
#define BINARY_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Binary Protocol Definitions
 *
 * Wire-format structures for high-performance binary messaging.
 * All multi-byte integers are in network byte order (big-endian).
 *
 * Design principles:
 * - Packed structs for minimal wire size
 * - Magic byte for protocol identification
 * - Fixed-size messages for predictable parsing
 * - Static assertions verify sizes at compile time
 */

/* ============================================================================
 * Protocol Constants
 * ============================================================================ */

/* Magic byte to identify binary protocol messages */
#define BINARY_MAGIC 0x4D  /* 'M' for Match */

/* Binary message types - Input */
#define BINARY_MSG_NEW_ORDER    'N'
#define BINARY_MSG_CANCEL       'C'
#define BINARY_MSG_FLUSH        'F'

/* Binary message types - Output */
#define BINARY_MSG_ACK          'A'
#define BINARY_MSG_CANCEL_ACK   'X'
#define BINARY_MSG_TRADE        'T'
#define BINARY_MSG_TOP_OF_BOOK  'B'

/* Maximum symbol length in binary protocol */
#define BINARY_SYMBOL_LEN 8

/* ============================================================================
 * Input Message Structures (Client -> Server)
 * ============================================================================ */

/**
 * Binary New Order Message
 *
 * Wire format (30 bytes):
 *   Offset 0:  magic (1 byte)
 *   Offset 1:  msg_type 'N' (1 byte)
 *   Offset 2:  user_id (4 bytes, network order)
 *   Offset 6:  symbol (8 bytes, null-padded)
 *   Offset 14: price (4 bytes, network order)
 *   Offset 18: quantity (4 bytes, network order)
 *   Offset 22: side 'B'/'S' (1 byte)
 *   Offset 23: user_order_id (4 bytes, network order)
 *   Total: 27 bytes (but packed may add padding - verify with static assert)
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;                      /* 0x4D */
    uint8_t  msg_type;                   /* 'N' */
    uint32_t user_id;                    /* Network byte order */
    char     symbol[BINARY_SYMBOL_LEN];  /* Fixed 8 chars, null-padded */
    uint32_t price;                      /* Network byte order */
    uint32_t quantity;                   /* Network byte order */
    uint8_t  side;                       /* 'B' or 'S' */
    uint32_t user_order_id;              /* Network byte order */
} binary_new_order_t;

_Static_assert(sizeof(binary_new_order_t) == 27,
               "binary_new_order_t must be 27 bytes");

/**
 * Binary Cancel Message
 *
 * Wire format (10 bytes):
 *   Offset 0: magic (1 byte)
 *   Offset 1: msg_type 'C' (1 byte)
 *   Offset 2: user_id (4 bytes, network order)
 *   Offset 6: user_order_id (4 bytes, network order)
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;              /* 0x4D */
    uint8_t  msg_type;           /* 'C' */
    uint32_t user_id;            /* Network byte order */
    uint32_t user_order_id;      /* Network byte order */
} binary_cancel_t;

_Static_assert(sizeof(binary_cancel_t) == 10,
               "binary_cancel_t must be 10 bytes");

/**
 * Binary Flush Message
 *
 * Wire format (2 bytes):
 *   Offset 0: magic (1 byte)
 *   Offset 1: msg_type 'F' (1 byte)
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;              /* 0x4D */
    uint8_t  msg_type;           /* 'F' */
} binary_flush_t;

_Static_assert(sizeof(binary_flush_t) == 2,
               "binary_flush_t must be 2 bytes");

/* ============================================================================
 * Output Message Structures (Server -> Client)
 * ============================================================================ */

/**
 * Binary Acknowledgement Message
 *
 * Wire format (18 bytes):
 *   Offset 0:  magic (1 byte)
 *   Offset 1:  msg_type 'A' (1 byte)
 *   Offset 2:  symbol (8 bytes)
 *   Offset 10: user_id (4 bytes, network order)
 *   Offset 14: user_order_id (4 bytes, network order)
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;                      /* 0x4D */
    uint8_t  msg_type;                   /* 'A' */
    char     symbol[BINARY_SYMBOL_LEN];
    uint32_t user_id;                    /* Network byte order */
    uint32_t user_order_id;              /* Network byte order */
} binary_ack_t;

_Static_assert(sizeof(binary_ack_t) == 18,
               "binary_ack_t must be 18 bytes");

/**
 * Binary Cancel Acknowledgement Message
 *
 * Wire format (18 bytes):
 *   Offset 0:  magic (1 byte)
 *   Offset 1:  msg_type 'X' (1 byte)
 *   Offset 2:  symbol (8 bytes)
 *   Offset 10: user_id (4 bytes, network order)
 *   Offset 14: user_order_id (4 bytes, network order)
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;                      /* 0x4D */
    uint8_t  msg_type;                   /* 'X' */
    char     symbol[BINARY_SYMBOL_LEN];
    uint32_t user_id;                    /* Network byte order */
    uint32_t user_order_id;              /* Network byte order */
} binary_cancel_ack_t;

_Static_assert(sizeof(binary_cancel_ack_t) == 18,
               "binary_cancel_ack_t must be 18 bytes");

/**
 * Binary Trade Message
 *
 * Wire format (34 bytes):
 *   Offset 0:  magic (1 byte)
 *   Offset 1:  msg_type 'T' (1 byte)
 *   Offset 2:  symbol (8 bytes)
 *   Offset 10: user_id_buy (4 bytes, network order)
 *   Offset 14: user_order_id_buy (4 bytes, network order)
 *   Offset 18: user_id_sell (4 bytes, network order)
 *   Offset 22: user_order_id_sell (4 bytes, network order)
 *   Offset 26: price (4 bytes, network order)
 *   Offset 30: quantity (4 bytes, network order)
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;                      /* 0x4D */
    uint8_t  msg_type;                   /* 'T' */
    char     symbol[BINARY_SYMBOL_LEN];
    uint32_t user_id_buy;                /* Network byte order */
    uint32_t user_order_id_buy;          /* Network byte order */
    uint32_t user_id_sell;               /* Network byte order */
    uint32_t user_order_id_sell;         /* Network byte order */
    uint32_t price;                      /* Network byte order */
    uint32_t quantity;                   /* Network byte order */
} binary_trade_t;

_Static_assert(sizeof(binary_trade_t) == 34,
               "binary_trade_t must be 34 bytes");

/**
 * Binary Top-of-Book Message
 *
 * Wire format (19 bytes):
 *   Offset 0:  magic (1 byte)
 *   Offset 1:  msg_type 'B' (1 byte)
 *   Offset 2:  symbol (8 bytes)
 *   Offset 10: side 'B'/'S' (1 byte)
 *   Offset 11: price (4 bytes, network order, 0 = eliminated)
 *   Offset 15: quantity (4 bytes, network order)
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;                      /* 0x4D */
    uint8_t  msg_type;                   /* 'B' */
    char     symbol[BINARY_SYMBOL_LEN];
    uint8_t  side;                       /* 'B' or 'S' */
    uint32_t price;                      /* Network byte order, 0 for eliminated */
    uint32_t quantity;                   /* Network byte order */
} binary_top_of_book_t;

_Static_assert(sizeof(binary_top_of_book_t) == 19,
               "binary_top_of_book_t must be 19 bytes");

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Check if data starts with binary protocol magic byte
 *
 * @param data  Pointer to message data
 * @param len   Length of data
 * @return true if data appears to be binary protocol
 *
 * Preconditions:
 * - len == 0 is valid (returns false)
 * - data may be NULL if len == 0
 */
static inline bool is_binary_message(const void* data, size_t len) {
    /* Allow NULL data if len is 0 */
    if (len < 2) {
        return false;
    }

    assert(data != NULL && "NULL data with non-zero length");

    const uint8_t* bytes = (const uint8_t*)data;
    return bytes[0] == BINARY_MAGIC;
}

/**
 * Get expected size for a binary message type
 *
 * @param msg_type  The message type byte ('N', 'C', 'F', etc.)
 * @return Expected message size, or 0 if unknown type
 */
static inline size_t binary_message_size(uint8_t msg_type) {
    switch (msg_type) {
        /* Input messages */
        case BINARY_MSG_NEW_ORDER:   return sizeof(binary_new_order_t);
        case BINARY_MSG_CANCEL:      return sizeof(binary_cancel_t);
        case BINARY_MSG_FLUSH:       return sizeof(binary_flush_t);
        /* Output messages */
        case BINARY_MSG_ACK:         return sizeof(binary_ack_t);
        case BINARY_MSG_CANCEL_ACK:  return sizeof(binary_cancel_ack_t);
        case BINARY_MSG_TRADE:       return sizeof(binary_trade_t);
        case BINARY_MSG_TOP_OF_BOOK: return sizeof(binary_top_of_book_t);
        default:                     return 0;
    }
}

/**
 * Validate binary message has correct magic and sufficient length
 *
 * @param data  Pointer to message data
 * @param len   Length of data
 * @return true if message appears valid
 */
static inline bool binary_message_validate(const void* data, size_t len) {
    if (len < 2) {
        return false;
    }

    assert(data != NULL && "NULL data with non-zero length");

    const uint8_t* bytes = (const uint8_t*)data;

    if (bytes[0] != BINARY_MAGIC) {
        return false;
    }

    size_t expected = binary_message_size(bytes[1]);
    return expected > 0 && len >= expected;
}

#ifdef __cplusplus
}
#endif

#endif /* BINARY_PROTOCOL_H */

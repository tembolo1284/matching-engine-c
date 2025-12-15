#ifndef BINARY_PROTOCOL_H
#define BINARY_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

/* Magic byte to identify binary protocol messages */
#define BINARY_MAGIC 0x4D  /* 'M' for Match */

/* Binary message types */
#define BINARY_MSG_NEW_ORDER    'N'
#define BINARY_MSG_CANCEL       'C'
#define BINARY_MSG_FLUSH        'F'

/* Binary output message types */
#define BINARY_MSG_ACK          'A'
#define BINARY_MSG_CANCEL_ACK   'X'
#define BINARY_MSG_TRADE        'T'
#define BINARY_MSG_TOP_OF_BOOK  'B'

/* Maximum symbol length in binary protocol */
#define BINARY_SYMBOL_LEN 8

/**
 * Binary New Order Message
 * Total size: 30 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;              /* 0x4D */
    uint8_t  msg_type;           /* 'N' */
    uint32_t user_id;            /* Network byte order */
    char     symbol[BINARY_SYMBOL_LEN];  /* Fixed 8 chars, null-padded */
    uint32_t price;              /* Network byte order */
    uint32_t quantity;           /* Network byte order */
    uint8_t  side;               /* 'B' or 'S' */
    uint32_t user_order_id;      /* Network byte order */
} binary_new_order_t;

/**
 * Binary Cancel Message
 * Total size: 11 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;              /* 0x4D */
    uint8_t  msg_type;           /* 'C' */
    uint32_t user_id;            /* Network byte order */
    uint32_t user_order_id;      /* Network byte order */
} binary_cancel_t;

/**
 * Binary Flush Message
 * Total size: 2 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;              /* 0x4D */
    uint8_t  msg_type;           /* 'F' */
} binary_flush_t;

/**
 * Binary Acknowledgement Message
 * Total size: 18 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;              /* 0x4D */
    uint8_t  msg_type;           /* 'A' */
    char     symbol[BINARY_SYMBOL_LEN];
    uint32_t user_id;            /* Network byte order */
    uint32_t user_order_id;      /* Network byte order */
} binary_ack_t;

/**
 * Binary Cancel Acknowledgement Message
 * Total size: 18 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;              /* 0x4D */
    uint8_t  msg_type;           /* 'X' */
    char     symbol[BINARY_SYMBOL_LEN];
    uint32_t user_id;            /* Network byte order */
    uint32_t user_order_id;      /* Network byte order */
} binary_cancel_ack_t;

/**
 * Binary Trade Message
 * Total size: 34 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;              /* 0x4D */
    uint8_t  msg_type;           /* 'T' */
    char     symbol[BINARY_SYMBOL_LEN];
    uint32_t user_id_buy;        /* Network byte order */
    uint32_t user_order_id_buy;  /* Network byte order */
    uint32_t user_id_sell;       /* Network byte order */
    uint32_t user_order_id_sell; /* Network byte order */
    uint32_t price;              /* Network byte order */
    uint32_t quantity;           /* Network byte order */
} binary_trade_t;

/**
 * Binary Top-of-Book Message
 * Total size: 20 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;              /* 0x4D */
    uint8_t  msg_type;           /* 'B' */
    char     symbol[BINARY_SYMBOL_LEN];
    uint8_t  side;               /* 'B' or 'S' */
    uint32_t price;              /* Network byte order, 0 for eliminated */
    uint32_t quantity;           /* Network byte order */
    uint8_t _padding;
} binary_top_of_book_t;

/**
 * Helper: Detect if message is binary protocol
 */
static inline bool is_binary_message(const char* data, size_t len) {
    return (len >= 2 && (uint8_t)data[0] == BINARY_MAGIC);
}

#endif /* BINARY_PROTOCOL_H */

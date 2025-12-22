#ifndef TRANSPORT_TYPES_H
#define TRANSPORT_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Transport Types - Shared structures for network transport abstraction
 *
 * These types are used by both socket and DPDK implementations.
 * They provide a common interface for packet handling regardless
 * of the underlying transport mechanism.
 */

/* ============================================================================
 * Client Address Types
 * ============================================================================ */

/**
 * UDP client address (compact representation)
 *
 * Used for client tracking in hash tables.
 * 8 bytes total for cache efficiency.
 */
typedef struct {
    uint32_t ip_addr;       /* Network byte order */
    uint16_t port;          /* Network byte order */
    uint16_t _pad;          /* Alignment padding */
} transport_addr_t;

_Static_assert(sizeof(transport_addr_t) == 8, "transport_addr_t must be 8 bytes");

/**
 * Compare two transport addresses
 */
static inline bool transport_addr_equal(const transport_addr_t* a,
                                         const transport_addr_t* b) {
    return a->ip_addr == b->ip_addr && a->port == b->port;
}

/**
 * Hash a transport address (for hash tables)
 */
static inline uint32_t transport_addr_hash(const transport_addr_t* addr) {
    uint32_t h = 2166136261u;  /* FNV-1a */
    h ^= addr->ip_addr;
    h *= 16777619u;
    h ^= addr->port;
    h *= 16777619u;
    return h;
}

/**
 * Convert sockaddr_in to transport_addr
 */
static inline void transport_addr_from_sockaddr(transport_addr_t* dst,
                                                 const struct sockaddr_in* src) {
    dst->ip_addr = src->sin_addr.s_addr;
    dst->port = src->sin_port;
    dst->_pad = 0;
}

/**
 * Convert transport_addr to sockaddr_in
 */
static inline void transport_addr_to_sockaddr(struct sockaddr_in* dst,
                                               const transport_addr_t* src) {
    dst->sin_family = AF_INET;
    dst->sin_addr.s_addr = src->ip_addr;
    dst->sin_port = src->port;
}

/* ============================================================================
 * Protocol Detection
 * ============================================================================ */

/**
 * Client protocol type (auto-detected)
 */
typedef enum {
    TRANSPORT_PROTO_UNKNOWN = 0,
    TRANSPORT_PROTO_CSV     = 1,
    TRANSPORT_PROTO_BINARY  = 2
} transport_protocol_t;

/* ============================================================================
 * Received Packet Structure
 * ============================================================================ */

/**
 * Maximum packet size for UDP
 *
 * MTU (1500) - IP header (20) - UDP header (8) = 1472 bytes typical
 * We use a larger buffer for jumbo frames or local testing.
 */
#define TRANSPORT_MAX_PACKET_SIZE 65507

/**
 * Received packet (zero-copy where possible)
 *
 * For socket implementation: data points to recv buffer
 * For DPDK implementation: data points to rte_mbuf payload
 */
typedef struct {
    const uint8_t* data;        /* Packet payload (not owned) */
    size_t len;                 /* Payload length */
    transport_addr_t src_addr;  /* Source address */
    uint64_t timestamp;         /* Receive timestamp (rdtsc or equivalent) */
} transport_rx_packet_t;

/**
 * Transmit packet structure
 */
typedef struct {
    const uint8_t* data;        /* Packet payload (not owned) */
    size_t len;                 /* Payload length */
    transport_addr_t dst_addr;  /* Destination address */
} transport_tx_packet_t;

/* ============================================================================
 * Statistics Structure
 * ============================================================================ */

/**
 * Transport statistics (common to all implementations)
 */
typedef struct {
    /* Receive stats */
    uint64_t rx_packets;        /* Total packets received */
    uint64_t rx_bytes;          /* Total bytes received */
    uint64_t rx_messages;       /* Parsed messages (may differ from packets) */
    uint64_t rx_errors;         /* Receive errors */
    uint64_t rx_dropped;        /* Dropped packets (queue full, etc.) */

    /* Transmit stats */
    uint64_t tx_packets;        /* Total packets sent */
    uint64_t tx_bytes;          /* Total bytes sent */
    uint64_t tx_errors;         /* Transmit errors */

    /* Client tracking */
    uint32_t active_clients;    /* Current connected/tracked clients */
    uint32_t peak_clients;      /* Peak concurrent clients */

    /* DPDK-specific (zero for socket implementation) */
    uint64_t rx_poll_empty;     /* Empty poll cycles (DPDK only) */
    uint64_t rx_poll_full;      /* Full burst cycles (DPDK only) */
    uint64_t tx_batch_count;    /* TX batches sent (DPDK only) */
} transport_stats_t;

/**
 * Reset statistics to zero
 */
static inline void transport_stats_reset(transport_stats_t* stats) {
    stats->rx_packets = 0;
    stats->rx_bytes = 0;
    stats->rx_messages = 0;
    stats->rx_errors = 0;
    stats->rx_dropped = 0;
    stats->tx_packets = 0;
    stats->tx_bytes = 0;
    stats->tx_errors = 0;
    /* Don't reset client counts - they're current state, not cumulative */
    stats->rx_poll_empty = 0;
    stats->rx_poll_full = 0;
    stats->tx_batch_count = 0;
}

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/**
 * Default buffer sizes
 */
#define TRANSPORT_DEFAULT_RX_BUFFER_SIZE  (10 * 1024 * 1024)  /* 10 MB */
#define TRANSPORT_DEFAULT_TX_BUFFER_SIZE  (4 * 1024 * 1024)   /* 4 MB */

/**
 * Default timeouts
 */
#define TRANSPORT_DEFAULT_RX_TIMEOUT_US   100000  /* 100ms */
#define TRANSPORT_CLIENT_TIMEOUT_SEC      300     /* 5 minutes */

/**
 * Client limits
 */
#define TRANSPORT_MAX_CLIENTS             4096
#define TRANSPORT_CLIENT_HASH_SIZE        8192    /* 2x max for load factor */

_Static_assert((TRANSPORT_CLIENT_HASH_SIZE & (TRANSPORT_CLIENT_HASH_SIZE - 1)) == 0,
               "Hash size must be power of 2");

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_TYPES_H */

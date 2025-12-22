#ifndef MULTICAST_TRANSPORT_H
#define MULTICAST_TRANSPORT_H

#include "network/transport_types.h"
#include "protocol/message_types_extended.h"
#include "threading/queues.h"

#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Multicast Transport - Abstract Interface
 *
 * This header defines the interface for multicast packet transmission.
 * Used for broadcasting market data to multiple subscribers simultaneously.
 *
 * Two implementations are provided:
 *
 *   1. Socket implementation (default) - Uses IP multicast sockets
 *      - Works on any system with multicast support
 *      - Kernel handles multicast group management
 *      - Latency: ~10-50µs
 *
 *   2. DPDK implementation (USE_DPDK=1) - Direct NIC multicast
 *      - Constructs multicast MAC directly (01:00:5e:xx:xx:xx)
 *      - Bypasses kernel multicast stack
 *      - Latency: ~200ns
 *
 * Architecture:
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │                      Output Queues                             │
 *   │   Processor 0 ────► Output Queue 0 ──┐                        │
 *   │                                       ├──► Multicast Transport │
 *   │   Processor 1 ────► Output Queue 1 ──┘                        │
 *   └────────────────────────────────────────────────────────────────┘
 *                                │
 *                                ▼
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │              Multicast Transport Interface                     │
 *   │                                                                │
 *   │  multicast_transport_create()                                  │
 *   │  multicast_transport_start()    ← Spawns publisher thread     │
 *   │  multicast_transport_stop()                                    │
 *   │  multicast_transport_destroy()                                 │
 *   └────────────────────────────────────────────────────────────────┘
 *                                │
 *                                ▼
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │                    Multicast Group                             │
 *   │                    239.255.0.1:5000                            │
 *   │                          │                                     │
 *   │     ┌────────────────────┼────────────────────┐               │
 *   │     ▼                    ▼                    ▼               │
 *   │  Subscriber 1      Subscriber 2         Subscriber N          │
 *   │  (Market Maker)    (Algo Trader)        (Risk System)         │
 *   └────────────────────────────────────────────────────────────────┘
 */

/* ============================================================================
 * Forward Declaration
 * ============================================================================ */

/**
 * Opaque transport handle
 */
typedef struct multicast_transport multicast_transport_t;

/* ============================================================================
 * Configuration
 * ============================================================================ */

/**
 * Multicast transport configuration
 */
typedef struct {
    /* === Multicast Group === */
    const char* group_addr;         /* Multicast IP (e.g., "239.255.0.1") */
    uint16_t port;                  /* Destination port */

    /* === Protocol === */
    bool use_binary;                /* true = binary, false = CSV */

    /* === TTL and Scope === */
    uint8_t ttl;                    /* Time-to-live (1=local, 32=site, 255=global) */
    bool loopback;                  /* Receive own packets (for testing) */

    /* === Interface Selection === */
    const char* interface_addr;     /* Source interface IP (NULL = default) */

    /* === Performance === */
    uint32_t tx_buffer_size;        /* Socket send buffer (0 = default) */

    /* === DPDK-Specific (ignored for socket backend) === */
    uint16_t dpdk_port_id;          /* DPDK port ID */
    uint16_t dpdk_tx_queue;         /* TX queue to use */

} multicast_transport_config_t;

/**
 * Default TTL values
 */
#define MULTICAST_TTL_LOCAL     1       /* Same subnet only */
#define MULTICAST_TTL_SITE      32      /* Within organization */
#define MULTICAST_TTL_REGION    64      /* Regional */
#define MULTICAST_TTL_GLOBAL    255     /* Unrestricted */

/**
 * Initialize config with sensible defaults
 */
static inline void multicast_transport_config_init(multicast_transport_config_t* config) {
    config->group_addr = "239.255.0.1";
    config->port = 5000;
    config->use_binary = false;     /* CSV by default for debugging */
    config->ttl = MULTICAST_TTL_SITE;
    config->loopback = false;
    config->interface_addr = NULL;  /* Use default interface */
    config->tx_buffer_size = TRANSPORT_DEFAULT_TX_BUFFER_SIZE;
    config->dpdk_port_id = 0;
    config->dpdk_tx_queue = 0;
}

/* ============================================================================
 * Lifecycle API
 * ============================================================================ */

/**
 * Create multicast transport
 *
 * Allocates and initializes transport. Does NOT start publishing yet.
 *
 * @param config         Transport configuration
 * @param output_queue_0 Primary output queue (from processor 0)
 * @param output_queue_1 Secondary output queue (from processor 1, NULL if single)
 * @param shutdown_flag  Shared shutdown flag for graceful termination
 * @return Transport handle, or NULL on error
 *
 * Preconditions:
 * - config != NULL
 * - config->group_addr is valid multicast address (224.0.0.0-239.255.255.255)
 * - config->port > 0
 * - output_queue_0 != NULL
 * - shutdown_flag != NULL
 */
multicast_transport_t* multicast_transport_create(
    const multicast_transport_config_t* config,
    output_envelope_queue_t* output_queue_0,
    output_envelope_queue_t* output_queue_1,
    atomic_bool* shutdown_flag);

/**
 * Start multicast publisher
 *
 * Spawns publisher thread that drains output queues and broadcasts
 * to the multicast group.
 *
 * @param transport Transport handle
 * @return true on success, false on error
 */
bool multicast_transport_start(multicast_transport_t* transport);

/**
 * Stop multicast publisher
 *
 * Signals shutdown and waits for publisher to drain remaining messages.
 *
 * @param transport Transport handle
 */
void multicast_transport_stop(multicast_transport_t* transport);

/**
 * Destroy transport and free resources
 *
 * @param transport Transport handle (may be NULL)
 */
void multicast_transport_destroy(multicast_transport_t* transport);

/* ============================================================================
 * Direct Send API (optional - mainly for testing)
 * ============================================================================ */

/**
 * Send raw data to multicast group
 *
 * Bypasses output queues - sends directly. Useful for control messages.
 *
 * @param transport Transport handle
 * @param data      Packet data
 * @param len       Packet length
 * @return true on success, false on error
 */
bool multicast_transport_send(multicast_transport_t* transport,
                               const void* data,
                               size_t len);

/**
 * Send formatted output message to multicast group
 *
 * Formats message according to configured protocol and sends.
 *
 * @param transport Transport handle
 * @param msg       Output message to send
 * @return true on success, false on error
 */
bool multicast_transport_send_message(multicast_transport_t* transport,
                                       const output_msg_t* msg);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Multicast-specific statistics
 */
typedef struct {
    /* Transmit stats */
    uint64_t tx_packets;            /* Total packets sent */
    uint64_t tx_bytes;              /* Total bytes sent */
    uint64_t tx_messages;           /* Messages broadcast */
    uint64_t tx_errors;             /* Send errors */

    /* Queue stats */
    uint64_t messages_from_queue_0; /* Messages from processor 0 */
    uint64_t messages_from_queue_1; /* Messages from processor 1 */

    /* Format stats */
    uint64_t format_errors;         /* Message formatting errors */

    /* Sequence tracking */
    uint64_t sequence;              /* Current sequence number */

} multicast_transport_stats_t;

/**
 * Get multicast statistics
 */
void multicast_transport_get_stats(const multicast_transport_t* transport,
                                    multicast_transport_stats_t* stats);

/**
 * Reset multicast statistics (except sequence)
 */
void multicast_transport_reset_stats(multicast_transport_t* transport);

/**
 * Print multicast statistics to stderr
 */
void multicast_transport_print_stats(const multicast_transport_t* transport);

/* ============================================================================
 * Query API
 * ============================================================================ */

/**
 * Check if transport is running
 */
bool multicast_transport_is_running(const multicast_transport_t* transport);

/**
 * Get current sequence number
 *
 * Useful for gap detection on subscriber side.
 */
uint64_t multicast_transport_get_sequence(const multicast_transport_t* transport);

/**
 * Validate multicast address
 *
 * Checks if address is in valid multicast range (224.0.0.0-239.255.255.255).
 *
 * @param addr Address string (e.g., "239.255.0.1")
 * @return true if valid multicast address
 */
bool multicast_address_is_valid(const char* addr);

/**
 * Get implementation name
 *
 * @return "socket" or "dpdk"
 */
const char* multicast_transport_get_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* MULTICAST_TRANSPORT_H */

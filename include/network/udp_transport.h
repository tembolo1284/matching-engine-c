#ifndef UDP_TRANSPORT_H
#define UDP_TRANSPORT_H

#include "network/transport_types.h"
#include "protocol/message_types_extended.h"
#include "threading/queues.h"

#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * UDP Transport - Abstract Interface
 *
 * This header defines the interface for UDP packet I/O. Two implementations
 * are provided:
 *
 *   1. Socket implementation (default) - Uses standard POSIX sockets
 *      - Works on any POSIX system (Linux, macOS, BSD)
 *      - No special setup required
 *      - Latency: ~5-50µs
 *
 *   2. DPDK implementation (USE_DPDK=1) - Kernel bypass
 *      - Requires Linux with DPDK + compatible NIC
 *      - Requires huge pages and NIC binding
 *      - Latency: ~200ns
 *
 * Both implementations provide identical API - selection is compile-time:
 *
 *   cmake .. -DUSE_DPDK=OFF   # Socket implementation (default)
 *   cmake .. -DUSE_DPDK=ON    # DPDK implementation
 *
 * Architecture:
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                     Application Code                        │
 *   │                  (modes/unified_server.c)                   │
 *   └─────────────────────────┬───────────────────────────────────┘
 *                             │
 *                             ▼
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │              UDP Transport Interface (this file)            │
 *   │                                                             │
 *   │  udp_transport_create()     udp_transport_send()           │
 *   │  udp_transport_start()      udp_transport_send_to_client() │
 *   │  udp_transport_stop()       udp_transport_get_stats()      │
 *   │  udp_transport_destroy()                                    │
 *   └─────────────────────────┬───────────────────────────────────┘
 *                             │
 *              ┌──────────────┴──────────────┐
 *              │                             │
 *              ▼                             ▼
 *   ┌─────────────────────┐       ┌─────────────────────┐
 *   │  Socket Backend     │       │  DPDK Backend       │
 *   │  (udp_socket.c)     │       │  (dpdk_udp.c)       │
 *   │                     │       │                     │
 *   │  recvfrom/sendto    │       │  rte_eth_rx_burst   │
 *   │  select/poll        │       │  rte_eth_tx_burst   │
 *   └─────────────────────┘       └─────────────────────┘
 */

/* ============================================================================
 * Forward Declaration
 * ============================================================================ */

/**
 * Opaque transport handle
 *
 * Implementation details hidden - allows socket and DPDK to have
 * different internal structures.
 */
typedef struct udp_transport udp_transport_t;

/* ============================================================================
 * Configuration
 * ============================================================================ */

/**
 * UDP transport configuration
 *
 * Common configuration for both socket and DPDK implementations.
 * DPDK-specific fields are ignored when USE_DPDK=0.
 */
typedef struct {
    /* === Network Configuration === */
    uint16_t bind_port;             /* Port to bind (required) */
    const char* bind_addr;          /* Bind address (NULL = INADDR_ANY) */

    /* === Processing Configuration === */
    bool dual_processor;            /* Route to 2 queues based on symbol */
    bool detect_protocol;           /* Auto-detect binary vs CSV */
    transport_protocol_t default_protocol;  /* Default if not detected */

    /* === Performance Tuning === */
    uint32_t rx_buffer_size;        /* Socket receive buffer (0 = default) */
    uint32_t tx_buffer_size;        /* Socket send buffer (0 = default) */
    uint32_t rx_timeout_us;         /* Receive timeout in microseconds */
    bool busy_poll;                 /* Enable SO_BUSY_POLL (Linux only) */

    /* === DPDK-Specific (ignored for socket backend) === */
    const char* dpdk_eal_args;      /* EAL initialization arguments */
    uint16_t dpdk_port_id;          /* DPDK port ID */
    uint16_t dpdk_rx_queues;        /* Number of RX queues */
    uint16_t dpdk_tx_queues;        /* Number of TX queues */
    uint16_t dpdk_rx_ring_size;     /* RX descriptor ring size */
    uint16_t dpdk_tx_ring_size;     /* TX descriptor ring size */
    uint32_t dpdk_mempool_size;     /* Packet mempool size */
    uint16_t dpdk_mempool_cache;    /* Per-core mempool cache */

} udp_transport_config_t;

/**
 * Initialize config with sensible defaults
 */
static inline void udp_transport_config_init(udp_transport_config_t* config) {
    config->bind_port = 0;          /* Must be set by caller */
    config->bind_addr = NULL;       /* INADDR_ANY */
    config->dual_processor = false;
    config->detect_protocol = true;
    config->default_protocol = TRANSPORT_PROTO_CSV;
    config->rx_buffer_size = TRANSPORT_DEFAULT_RX_BUFFER_SIZE;
    config->tx_buffer_size = TRANSPORT_DEFAULT_TX_BUFFER_SIZE;
    config->rx_timeout_us = TRANSPORT_DEFAULT_RX_TIMEOUT_US;
    config->busy_poll = true;       /* Enable if available */

    /* DPDK defaults */
    config->dpdk_eal_args = NULL;
    config->dpdk_port_id = 0;
    config->dpdk_rx_queues = 1;
    config->dpdk_tx_queues = 1;
    config->dpdk_rx_ring_size = 1024;
    config->dpdk_tx_ring_size = 1024;
    config->dpdk_mempool_size = 8192;
    config->dpdk_mempool_cache = 256;
}

/* ============================================================================
 * Lifecycle API
 * ============================================================================ */

/**
 * Create UDP transport
 *
 * Allocates and initializes transport. Does NOT start receiving yet.
 *
 * @param config        Transport configuration
 * @param input_queue_0 Primary input queue (for processor 0 or single processor)
 * @param input_queue_1 Secondary input queue (for processor 1, NULL if single)
 * @param shutdown_flag Shared shutdown flag for graceful termination
 * @return Transport handle, or NULL on error
 *
 * Preconditions:
 * - config != NULL
 * - config->bind_port > 0
 * - input_queue_0 != NULL
 * - shutdown_flag != NULL
 * - If dual_processor, input_queue_1 != NULL
 */
udp_transport_t* udp_transport_create(const udp_transport_config_t* config,
                                       input_envelope_queue_t* input_queue_0,
                                       input_envelope_queue_t* input_queue_1,
                                       atomic_bool* shutdown_flag);

/**
 * Start receiving packets
 *
 * Spawns receiver thread (socket) or starts poll loop (DPDK).
 * Messages are parsed and routed to input queues.
 *
 * @param transport Transport handle
 * @return true on success, false on error
 *
 * Preconditions:
 * - transport != NULL
 * - Transport not already started
 */
bool udp_transport_start(udp_transport_t* transport);

/**
 * Stop receiving packets
 *
 * Signals shutdown and waits for receiver to finish.
 * Drains any remaining packets in flight.
 *
 * @param transport Transport handle
 *
 * Preconditions:
 * - transport != NULL
 */
void udp_transport_stop(udp_transport_t* transport);

/**
 * Destroy transport and free resources
 *
 * Stops transport if still running, then frees all resources.
 *
 * @param transport Transport handle (may be NULL, no-op if so)
 */
void udp_transport_destroy(udp_transport_t* transport);

/* ============================================================================
 * Send API
 * ============================================================================ */

/**
 * Send packet to specific client by ID
 *
 * Looks up client in registry and sends to their address.
 *
 * @param transport Transport handle
 * @param client_id Client identifier
 * @param data      Packet data
 * @param len       Packet length
 * @return true on success, false on error (client not found, send failed)
 *
 * Preconditions:
 * - transport != NULL
 * - data != NULL
 * - len > 0 && len <= TRANSPORT_MAX_PACKET_SIZE
 */
bool udp_transport_send_to_client(udp_transport_t* transport,
                                   uint32_t client_id,
                                   const void* data,
                                   size_t len);

/**
 * Send packet to specific address
 *
 * Sends directly to address without client lookup.
 *
 * @param transport Transport handle
 * @param addr      Destination address
 * @param data      Packet data
 * @param len       Packet length
 * @return true on success, false on error
 */
bool udp_transport_send_to_addr(udp_transport_t* transport,
                                 const transport_addr_t* addr,
                                 const void* data,
                                 size_t len);

/**
 * Send packet to last received address (fast path)
 *
 * Useful for request-response patterns where you want to reply
 * to whoever just sent you a message.
 *
 * @param transport Transport handle
 * @param data      Packet data
 * @param len       Packet length
 * @return true on success, false if no previous address or send failed
 */
bool udp_transport_send_to_last(udp_transport_t* transport,
                                 const void* data,
                                 size_t len);

/**
 * Broadcast to all known clients
 *
 * Sends same packet to all tracked clients. Use sparingly -
 * prefer multicast for market data distribution.
 *
 * @param transport Transport handle
 * @param data      Packet data
 * @param len       Packet length
 * @return Number of clients sent to (may be less than active_clients on errors)
 */
size_t udp_transport_broadcast(udp_transport_t* transport,
                                const void* data,
                                size_t len);

/* ============================================================================
 * Client Management
 * ============================================================================ */

/**
 * Get client address by ID
 *
 * @param transport Transport handle
 * @param client_id Client identifier
 * @param addr      Output: client address
 * @return true if client found, false otherwise
 */
bool udp_transport_get_client_addr(const udp_transport_t* transport,
                                    uint32_t client_id,
                                    transport_addr_t* addr);

/**
 * Get client protocol by ID
 *
 * @param transport Transport handle
 * @param client_id Client identifier
 * @return Protocol type, or UNKNOWN if client not found
 */
transport_protocol_t udp_transport_get_client_protocol(
    const udp_transport_t* transport,
    uint32_t client_id);

/**
 * Evict inactive clients
 *
 * Removes clients that haven't sent packets in timeout_sec seconds.
 *
 * @param transport   Transport handle
 * @param timeout_sec Inactivity timeout in seconds
 * @return Number of clients evicted
 */
size_t udp_transport_evict_inactive(udp_transport_t* transport,
                                     uint32_t timeout_sec);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Get transport statistics
 *
 * @param transport Transport handle
 * @param stats     Output: statistics structure
 */
void udp_transport_get_stats(const udp_transport_t* transport,
                              transport_stats_t* stats);

/**
 * Reset transport statistics
 *
 * @param transport Transport handle
 */
void udp_transport_reset_stats(udp_transport_t* transport);

/**
 * Print transport statistics to stderr
 *
 * @param transport Transport handle
 */
void udp_transport_print_stats(const udp_transport_t* transport);

/* ============================================================================
 * Query API
 * ============================================================================ */

/**
 * Check if transport is running
 */
bool udp_transport_is_running(const udp_transport_t* transport);

/**
 * Get bound port (useful if bind_port was 0)
 */
uint16_t udp_transport_get_port(const udp_transport_t* transport);

/**
 * Get implementation name
 *
 * @return "socket" or "dpdk"
 */
const char* udp_transport_get_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* UDP_TRANSPORT_H */

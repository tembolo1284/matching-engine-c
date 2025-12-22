#ifndef DPDK_CONFIG_H
#define DPDK_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DPDK Configuration
 *
 * Central configuration for DPDK initialization and operation.
 * These values are tuned for low-latency trading applications.
 */

/* ============================================================================
 * Build Configuration
 * ============================================================================ */

#ifndef USE_DPDK
#define USE_DPDK 0
#endif

/* ============================================================================
 * EAL (Environment Abstraction Layer) Configuration
 * ============================================================================ */

#define DPDK_DEFAULT_EAL_ARGS  "-l 0-1 -n 4 --proc-type=primary"
#define DPDK_MEMORY_CHANNELS   4

/* ============================================================================
 * Port Configuration
 * ============================================================================ */

#define DPDK_MAX_PORTS         4
#define DPDK_RX_RING_SIZE      512
#define DPDK_TX_RING_SIZE      512
#define DPDK_RX_BURST_SIZE     32
#define DPDK_TX_BURST_SIZE     32
#define DPDK_RX_QUEUES         1
#define DPDK_TX_QUEUES         1

/* ============================================================================
 * Mempool Configuration
 * ============================================================================ */

#define DPDK_NUM_MBUFS         8192
#define DPDK_MBUF_CACHE_SIZE   256
#define DPDK_MBUF_DATA_SIZE    2048

/* ============================================================================
 * Virtual Device Configuration
 * ============================================================================ */

typedef enum {
    DPDK_VDEV_NONE = 0,
    DPDK_VDEV_NULL,
    DPDK_VDEV_RING,
    DPDK_VDEV_PCAP,
} dpdk_vdev_type_t;

#define DPDK_VDEV_NULL_ARGS    "--vdev=net_null0"
#define DPDK_VDEV_RING_ARGS    "--vdev=net_ring0"

/* ============================================================================
 * Timing Configuration
 * ============================================================================ */

#define DPDK_POLL_TIMEOUT_US   0
#define DPDK_TX_DRAIN_US       100

/* ============================================================================
 * Protocol Configuration
 * ============================================================================ */

#define DPDK_ETHER_HDR_SIZE    14
#define DPDK_IPV4_HDR_SIZE     20
#define DPDK_UDP_HDR_SIZE      8
#define DPDK_HEADER_OVERHEAD   (DPDK_ETHER_HDR_SIZE + DPDK_IPV4_HDR_SIZE + DPDK_UDP_HDR_SIZE)
#define DPDK_MAX_UDP_PAYLOAD   1458

/* ============================================================================
 * Multicast Configuration
 * ============================================================================ */

#define DPDK_MCAST_MAC_PREFIX  0x01005e000000ULL

static inline uint64_t dpdk_mcast_ip_to_mac(uint32_t ip) {
    uint32_t host_ip = __builtin_bswap32(ip);
    uint32_t mac_suffix = host_ip & 0x007FFFFF;
    return DPDK_MCAST_MAC_PREFIX | mac_suffix;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

typedef struct {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_errors;
    uint64_t rx_missed;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_errors;
    uint64_t tx_dropped;
    uint64_t rx_polls;
    uint64_t rx_polls_empty;
    uint64_t rx_polls_full;
    uint64_t tx_batches;
    uint64_t tx_batch_avg_size;
} dpdk_stats_t;

/* ============================================================================
 * Runtime Configuration Structure
 * ============================================================================ */

typedef struct {
    const char* eal_args;
    uint16_t port_id;
    uint16_t rx_queues;
    uint16_t tx_queues;
    uint16_t rx_ring_size;
    uint16_t tx_ring_size;
    uint32_t num_mbufs;
    uint32_t mbuf_cache_size;
    dpdk_vdev_type_t vdev_type;
    bool promiscuous;
    bool multicast;
} dpdk_config_t;

static inline void dpdk_config_init(dpdk_config_t* config) {
    config->eal_args = NULL;
    config->port_id = 0;
    config->rx_queues = DPDK_RX_QUEUES;
    config->tx_queues = DPDK_TX_QUEUES;
    config->rx_ring_size = DPDK_RX_RING_SIZE;
    config->tx_ring_size = DPDK_TX_RING_SIZE;
    config->num_mbufs = DPDK_NUM_MBUFS;
    config->mbuf_cache_size = DPDK_MBUF_CACHE_SIZE;
    config->vdev_type = DPDK_VDEV_NONE;
    config->promiscuous = false;
    config->multicast = true;
}

static inline void dpdk_config_init_vdev(dpdk_config_t* config, 
                                          dpdk_vdev_type_t vdev_type) {
    dpdk_config_init(config);
    config->vdev_type = vdev_type;
    config->port_id = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* DPDK_CONFIG_H */

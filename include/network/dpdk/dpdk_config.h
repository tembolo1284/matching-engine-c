#ifndef DPDK_CONFIG_H
#define DPDK_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

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

/**
 * Default EAL arguments
 *
 * These can be overridden at runtime via dpdk_config_t.eal_args
 */
#define DPDK_DEFAULT_EAL_ARGS  "-l 0-1 -n 4 --proc-type=primary"

/**
 * Memory channels (typically 4 for modern systems)
 */
#define DPDK_MEMORY_CHANNELS   4

/* ============================================================================
 * Port Configuration
 * ============================================================================ */

/**
 * Maximum ports we'll handle
 */
#define DPDK_MAX_PORTS         4

/**
 * RX/TX descriptor ring sizes (must be power of 2)
 *
 * Larger = more buffering, higher throughput
 * Smaller = lower latency, less memory
 *
 * For ultra-low-latency: 256-512
 * For high throughput: 1024-4096
 */
#define DPDK_RX_RING_SIZE      512
#define DPDK_TX_RING_SIZE      512

/**
 * RX/TX burst size
 *
 * Number of packets to process in one burst.
 * Should be <= ring size / 4 for good performance.
 */
#define DPDK_RX_BURST_SIZE     32
#define DPDK_TX_BURST_SIZE     32

/**
 * Number of RX/TX queues per port
 *
 * More queues = better multi-core scaling
 * Typically 1 queue per core that processes packets
 */
#define DPDK_RX_QUEUES         1
#define DPDK_TX_QUEUES         1

/* ============================================================================
 * Mempool Configuration
 * ============================================================================ */

/**
 * Number of mbufs in the packet pool
 *
 * Should be enough for:
 *   - RX descriptors across all ports/queues
 *   - TX descriptors across all ports/queues
 *   - In-flight packets in application
 *   - Safety margin
 *
 * Formula: (rx_rings + tx_rings) * ring_size * ports + headroom
 */
#define DPDK_NUM_MBUFS         8192

/**
 * Per-core mbuf cache size
 *
 * Each core caches this many mbufs to avoid mempool contention.
 * Should be <= NUM_MBUFS / num_cores / 4
 */
#define DPDK_MBUF_CACHE_SIZE   256

/**
 * Mbuf data room size
 *
 * RTE_PKTMBUF_HEADROOM (128) + max packet size (typically 2048)
 * Default: RTE_MBUF_DEFAULT_DATAROOM = 2048
 */
#define DPDK_MBUF_DATA_SIZE    2048

/* ============================================================================
 * Virtual Device Configuration (for testing without physical NIC)
 * ============================================================================ */

/**
 * Virtual device types
 */
typedef enum {
    DPDK_VDEV_NONE = 0,      /* Use physical port */
    DPDK_VDEV_NULL,          /* net_null - drops all packets */
    DPDK_VDEV_RING,          /* net_ring - internal ring buffer */
    DPDK_VDEV_PCAP,          /* net_pcap - read/write pcap files */
} dpdk_vdev_type_t;

/**
 * Virtual device EAL arguments
 */
#define DPDK_VDEV_NULL_ARGS    "--vdev=net_null0"
#define DPDK_VDEV_RING_ARGS    "--vdev=net_ring0"

/* ============================================================================
 * Timing Configuration
 * ============================================================================ */

/**
 * Poll timeout in microseconds (0 = busy poll, no sleep)
 */
#define DPDK_POLL_TIMEOUT_US   0

/**
 * Drain TX buffer every N packets or this many microseconds
 */
#define DPDK_TX_DRAIN_US       100

/* ============================================================================
 * Protocol Configuration
 * ============================================================================ */

/**
 * Ethernet header size
 */
#define DPDK_ETHER_HDR_SIZE    14

/**
 * IPv4 header size (no options)
 */
#define DPDK_IPV4_HDR_SIZE     20

/**
 * UDP header size
 */
#define DPDK_UDP_HDR_SIZE      8

/**
 * Total header overhead
 */
#define DPDK_HEADER_OVERHEAD   (DPDK_ETHER_HDR_SIZE + DPDK_IPV4_HDR_SIZE + DPDK_UDP_HDR_SIZE)

/**
 * Maximum UDP payload
 *
 * MTU (1500) - headers (42) = 1458 typical
 * We use larger for jumbo frames or testing
 */
#define DPDK_MAX_UDP_PAYLOAD   1458

/* ============================================================================
 * Multicast Configuration
 * ============================================================================ */

/**
 * Multicast MAC prefix (01:00:5e)
 *
 * Lower 23 bits of multicast IP map to MAC:
 * IP: 239.255.0.1 → MAC: 01:00:5e:7f:00:01
 */
#define DPDK_MCAST_MAC_PREFIX  0x01005e000000ULL

/**
 * Convert multicast IP to MAC address
 *
 * @param ip Multicast IP in network byte order
 * @return MAC address as uint64_t (lower 48 bits)
 */
static inline uint64_t dpdk_mcast_ip_to_mac(uint32_t ip) {
    /* Lower 23 bits of IP map to lower 23 bits of MAC */
    uint32_t host_ip = __builtin_bswap32(ip);
    uint32_t mac_suffix = host_ip & 0x007FFFFF;
    return DPDK_MCAST_MAC_PREFIX | mac_suffix;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Extended DPDK statistics
 */
typedef struct {
    /* RX statistics */
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_errors;
    uint64_t rx_missed;          /* Packets missed due to no buffer */
    
    /* TX statistics */
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_errors;
    uint64_t tx_dropped;         /* Packets dropped (ring full) */
    
    /* Poll statistics */
    uint64_t rx_polls;           /* Total poll calls */
    uint64_t rx_polls_empty;     /* Polls that returned 0 packets */
    uint64_t rx_polls_full;      /* Polls that returned burst_size packets */
    
    /* Batch statistics */
    uint64_t tx_batches;         /* TX burst calls */
    uint64_t tx_batch_avg_size;  /* Average packets per burst */
    
} dpdk_stats_t;

/* ============================================================================
 * Runtime Configuration Structure
 * ============================================================================ */

/**
 * DPDK runtime configuration
 *
 * Passed to dpdk_init() to configure the DPDK environment.
 */
typedef struct {
    /* EAL arguments (NULL = use defaults) */
    const char* eal_args;
    
    /* Port configuration */
    uint16_t port_id;            /* Physical port to use (or 0 for vdev) */
    uint16_t rx_queues;          /* Number of RX queues */
    uint16_t tx_queues;          /* Number of TX queues */
    uint16_t rx_ring_size;       /* RX descriptor ring size */
    uint16_t tx_ring_size;       /* TX descriptor ring size */
    
    /* Mempool configuration */
    uint32_t num_mbufs;          /* Number of packet buffers */
    uint32_t mbuf_cache_size;    /* Per-core cache size */
    
    /* Virtual device (for testing without physical NIC) */
    dpdk_vdev_type_t vdev_type;  /* Type of virtual device */
    
    /* Application settings */
    bool promiscuous;            /* Enable promiscuous mode */
    bool multicast;              /* Enable multicast reception */
    
} dpdk_config_t;

/**
 * Initialize config with sensible defaults
 */
static inline void dpdk_config_init(dpdk_config_t* config) {
    config->eal_args = NULL;  /* Will use DPDK_DEFAULT_EAL_ARGS */
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

/**
 * Initialize config for virtual device testing (no physical NIC)
 */
static inline void dpdk_config_init_vdev(dpdk_config_t* config, 
                                          dpdk_vdev_type_t vdev_type) {
    dpdk_config_init(config);
    config->vdev_type = vdev_type;
    config->port_id = 0;  /* vdev is always port 0 */
}

#ifdef __cplusplus
}
#endif

#endif /* DPDK_CONFIG_H */

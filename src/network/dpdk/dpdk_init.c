#ifdef USE_DPDK

#include "network/dpdk/dpdk_init.h"
#include "network/dpdk/dpdk_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* DPDK headers */
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_lcore.h>
#include <rte_version.h>

/**
 * DPDK Initialization Implementation
 */

/* ============================================================================
 * Global State
 * ============================================================================ */

static struct {
    bool initialized;
    struct rte_mempool* mempool;
    uint16_t port_id;
    dpdk_config_t config;
} g_dpdk = {
    .initialized = false,
    .mempool = NULL,
    .port_id = 0,
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void mac_to_str(const uint8_t mac[6], char* buf) {
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ============================================================================
 * Port Setup
 * ============================================================================ */

static int setup_port(uint16_t port_id, const dpdk_config_t* config) {
    assert(config != NULL && "NULL config");
    
    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));
    
    /* Basic port configuration */
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
    
    /* Get port info */
    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        fprintf(stderr, "[DPDK] Failed to get device info: %d\n", ret);
        return -1;
    }
    
    fprintf(stderr, "[DPDK] Port %u: driver=%s\n", port_id, dev_info.driver_name);
    
    /* Configure port */
    ret = rte_eth_dev_configure(port_id, config->rx_queues, config->tx_queues, &port_conf);
    if (ret != 0) {
        fprintf(stderr, "[DPDK] Failed to configure port: %d\n", ret);
        return -4;
    }
    
    /* Setup RX queues */
    for (uint16_t q = 0; q < config->rx_queues; q++) {
        ret = rte_eth_rx_queue_setup(port_id, q, config->rx_ring_size,
                                      rte_eth_dev_socket_id(port_id),
                                      NULL, g_dpdk.mempool);
        if (ret != 0) {
            fprintf(stderr, "[DPDK] Failed to setup RX queue %u: %d\n", q, ret);
            return -5;
        }
    }
    
    /* Setup TX queues */
    for (uint16_t q = 0; q < config->tx_queues; q++) {
        ret = rte_eth_tx_queue_setup(port_id, q, config->tx_ring_size,
                                      rte_eth_dev_socket_id(port_id), NULL);
        if (ret != 0) {
            fprintf(stderr, "[DPDK] Failed to setup TX queue %u: %d\n", q, ret);
            return -6;
        }
    }
    
    /* Promiscuous mode */
    if (config->promiscuous) {
        ret = rte_eth_promiscuous_enable(port_id);
        if (ret != 0) {
            fprintf(stderr, "[DPDK] Warning: promiscuous mode failed: %d\n", ret);
        }
    }
    
    /* Start port */
    ret = rte_eth_dev_start(port_id);
    if (ret != 0) {
        fprintf(stderr, "[DPDK] Failed to start port: %d\n", ret);
        return -7;
    }
    
    /* Print MAC address */
    struct rte_ether_addr mac_addr;
    ret = rte_eth_macaddr_get(port_id, &mac_addr);
    if (ret == 0) {
        char mac_str[18];
        mac_to_str(mac_addr.addr_bytes, mac_str);
        fprintf(stderr, "[DPDK] Port %u MAC: %s\n", port_id, mac_str);
    }
    
    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int dpdk_init(const dpdk_config_t* config) {
    assert(config != NULL && "NULL config");
    
    if (g_dpdk.initialized) {
        fprintf(stderr, "[DPDK] Already initialized\n");
        return 0;
    }
    
    /* Build EAL arguments */
    const char* eal_args = config->eal_args ? config->eal_args : DPDK_DEFAULT_EAL_ARGS;
    
    /* Parse into argv format */
    char args_copy[1024];
    strncpy(args_copy, eal_args, sizeof(args_copy) - 1);
    args_copy[sizeof(args_copy) - 1] = '\0';
    
    char* argv[64];
    int argc = 0;
    argv[argc++] = (char*)"matching-engine";
    
    char* token = strtok(args_copy, " ");
    while (token != NULL && argc < 63) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;
    
    /* Add virtual device if configured */
    char vdev_arg[128] = "";
    if (config->vdev_type == DPDK_VDEV_NULL) {
        snprintf(vdev_arg, sizeof(vdev_arg), "--vdev=net_null0");
        argv[argc++] = vdev_arg;
    } else if (config->vdev_type == DPDK_VDEV_RING) {
        snprintf(vdev_arg, sizeof(vdev_arg), "--vdev=net_ring0");
        argv[argc++] = vdev_arg;
    }
    
    /* Initialize EAL */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "[DPDK] EAL init failed: %d\n", ret);
        return -1;
    }
    
    fprintf(stderr, "[DPDK] EAL initialized\n");
    
    /* Check available ports */
    uint16_t nb_ports = rte_eth_dev_count_avail();
    fprintf(stderr, "[DPDK] Available ports: %u\n", nb_ports);
    
    if (nb_ports == 0) {
        fprintf(stderr, "[DPDK] No ports available\n");
        return -2;
    }
    
    /* Create mempool */
    g_dpdk.mempool = rte_pktmbuf_pool_create("MBUF_POOL",
                                               config->num_mbufs,
                                               config->mbuf_cache_size,
                                               0,
                                               RTE_MBUF_DEFAULT_BUF_SIZE,
                                               rte_socket_id());
    
    if (g_dpdk.mempool == NULL) {
        fprintf(stderr, "[DPDK] Failed to create mempool\n");
        return -3;
    }
    
    fprintf(stderr, "[DPDK] Mempool created (%u mbufs)\n", config->num_mbufs);
    
    /* Setup port */
    g_dpdk.port_id = config->port_id;
    ret = setup_port(g_dpdk.port_id, config);
    if (ret != 0) {
        rte_mempool_free(g_dpdk.mempool);
        g_dpdk.mempool = NULL;
        return ret;
    }
    
    g_dpdk.config = *config;
    g_dpdk.initialized = true;
    
    fprintf(stderr, "[DPDK] Initialization complete\n");
    return 0;
}

int dpdk_init_default(void) {
    dpdk_config_t config;
    dpdk_config_init(&config);
    return dpdk_init(&config);
}

int dpdk_init_vdev(dpdk_vdev_type_t vdev_type) {
    dpdk_config_t config;
    dpdk_config_init_vdev(&config, vdev_type);
    return dpdk_init(&config);
}

void dpdk_cleanup(void) {
    if (!g_dpdk.initialized) {
        return;
    }
    
    /* Stop port */
    rte_eth_dev_stop(g_dpdk.port_id);
    rte_eth_dev_close(g_dpdk.port_id);
    
    /* Free mempool */
    if (g_dpdk.mempool != NULL) {
        rte_mempool_free(g_dpdk.mempool);
        g_dpdk.mempool = NULL;
    }
    
    /* Cleanup EAL */
    rte_eal_cleanup();
    
    g_dpdk.initialized = false;
    
    fprintf(stderr, "[DPDK] Cleanup complete\n");
}

struct rte_mempool* dpdk_get_mempool(void) {
    return g_dpdk.mempool;
}

uint16_t dpdk_get_port_id(void) {
    return g_dpdk.port_id;
}

bool dpdk_is_initialized(void) {
    return g_dpdk.initialized;
}

void dpdk_port_stop(void) {
    if (g_dpdk.initialized) {
        rte_eth_dev_stop(g_dpdk.port_id);
    }
}

int dpdk_port_start(void) {
    if (!g_dpdk.initialized) {
        return -1;
    }
    return rte_eth_dev_start(g_dpdk.port_id);
}

int dpdk_port_link_status(bool* link_up, uint32_t* speed_mbps) {
    assert(link_up != NULL && "NULL link_up");
    assert(speed_mbps != NULL && "NULL speed_mbps");
    
    if (!g_dpdk.initialized) {
        return -1;
    }
    
    struct rte_eth_link link;
    int ret = rte_eth_link_get_nowait(g_dpdk.port_id, &link);
    if (ret != 0) {
        return ret;
    }
    
    *link_up = (link.link_status == RTE_ETH_LINK_UP);
    *speed_mbps = link.link_speed;
    
    return 0;
}

int dpdk_get_stats(dpdk_stats_t* stats) {
    assert(stats != NULL && "NULL stats");
    
    if (!g_dpdk.initialized) {
        return -1;
    }
    
    struct rte_eth_stats eth_stats;
    int ret = rte_eth_stats_get(g_dpdk.port_id, &eth_stats);
    if (ret != 0) {
        return ret;
    }
    
    stats->rx_packets = eth_stats.ipackets;
    stats->rx_bytes = eth_stats.ibytes;
    stats->rx_errors = eth_stats.ierrors;
    stats->rx_missed = eth_stats.imissed;
    stats->tx_packets = eth_stats.opackets;
    stats->tx_bytes = eth_stats.obytes;
    stats->tx_errors = eth_stats.oerrors;
    stats->tx_dropped = 0;
    stats->rx_polls = 0;
    stats->rx_polls_empty = 0;
    stats->rx_polls_full = 0;
    stats->tx_batches = 0;
    stats->tx_batch_avg_size = 0;
    
    return 0;
}

int dpdk_reset_stats(void) {
    if (!g_dpdk.initialized) {
        return -1;
    }
    return rte_eth_stats_reset(g_dpdk.port_id);
}

void dpdk_print_stats(void) {
    if (!g_dpdk.initialized) {
        fprintf(stderr, "[DPDK] Not initialized\n");
        return;
    }
    
    dpdk_stats_t stats;
    if (dpdk_get_stats(&stats) != 0) {
        fprintf(stderr, "[DPDK] Failed to get stats\n");
        return;
    }
    
    fprintf(stderr, "\n=== DPDK Port %u Statistics ===\n", g_dpdk.port_id);
    fprintf(stderr, "RX packets:  %lu\n", stats.rx_packets);
    fprintf(stderr, "RX bytes:    %lu\n", stats.rx_bytes);
    fprintf(stderr, "RX errors:   %lu\n", stats.rx_errors);
    fprintf(stderr, "RX missed:   %lu\n", stats.rx_missed);
    fprintf(stderr, "TX packets:  %lu\n", stats.tx_packets);
    fprintf(stderr, "TX bytes:    %lu\n", stats.tx_bytes);
    fprintf(stderr, "TX errors:   %lu\n", stats.tx_errors);
}

const char* dpdk_version(void) {
    return rte_version();
}

const char* dpdk_strerror(int errnum) {
    switch (errnum) {
        case 0:  return "Success";
        case -1: return "EAL initialization failed";
        case -2: return "No ports available";
        case -3: return "Mempool creation failed";
        case -4: return "Port configuration failed";
        case -5: return "RX queue setup failed";
        case -6: return "TX queue setup failed";
        case -7: return "Port start failed";
        default: return "Unknown error";
    }
}

#endif /* USE_DPDK */

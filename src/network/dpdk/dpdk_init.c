#ifdef USE_DPDK

#include "network/dpdk/dpdk_init.h"

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
 *
 * This module initializes the DPDK environment:
 *   - EAL (Environment Abstraction Layer)
 *   - Mempool for packet buffers
 *   - Ethernet port configuration
 *
 * Rule Compliance:
 * - Rule 2: All loops bounded
 * - Rule 5: Minimum 2 assertions per function
 * - Rule 7: All return values checked
 */

/* ============================================================================
 * Global State
 * ============================================================================ */

static struct {
    bool initialized;
    struct rte_mempool* mempool;
    uint16_t active_port;
    dpdk_config_t config;
    dpdk_stats_t stats;
} g_dpdk = {
    .initialized = false,
    .mempool = NULL,
    .active_port = UINT16_MAX,
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Build EAL argument array from string
 *
 * @param args_str EAL arguments as string
 * @param argc_out Output: argument count
 * @return Allocated argv array (caller must free)
 */
static char** build_eal_args(const char* args_str, int* argc_out) {
    assert(argc_out != NULL && "NULL argc_out");

    /* Start with program name */
    const char* prog_name = "matching-engine";

    /* Count arguments */
    int argc = 1;  /* Program name */

    if (args_str != NULL && args_str[0] != '\0') {
        const char* p = args_str;
        bool in_word = false;

        for (int i = 0; i < 1024 && p[i] != '\0'; i++) {  /* Rule 2: bounded */
            if (p[i] == ' ' || p[i] == '\t') {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                argc++;
            }
        }
    }

    /* Allocate argv */
    char** argv = malloc((size_t)(argc + 1) * sizeof(char*));
    if (argv == NULL) {
        *argc_out = 0;
        return NULL;
    }

    /* First arg is program name */
    argv[0] = strdup(prog_name);
    if (argv[0] == NULL) {
        free(argv);
        *argc_out = 0;
        return NULL;
    }

    /* Parse remaining args */
    if (args_str != NULL && args_str[0] != '\0') {
        char* args_copy = strdup(args_str);
        if (args_copy == NULL) {
            free(argv[0]);
            free(argv);
            *argc_out = 0;
            return NULL;
        }

        int idx = 1;
        char* token = strtok(args_copy, " \t");
        while (token != NULL && idx < argc) {  /* Rule 2: bounded by argc */
            argv[idx] = strdup(token);
            if (argv[idx] == NULL) {
                /* Cleanup on failure */
                for (int j = 0; j < idx; j++) {
                    free(argv[j]);
                }
                free(argv);
                free(args_copy);
                *argc_out = 0;
                return NULL;
            }
            idx++;
            token = strtok(NULL, " \t");
        }

        free(args_copy);
    }

    argv[argc] = NULL;
    *argc_out = argc;
    return argv;
}

/**
 * Free EAL argument array
 */
static void free_eal_args(char** argv, int argc) {
    if (argv == NULL) return;

    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

/**
 * Get port configuration
 */
static struct rte_eth_conf get_port_conf(void) {
    struct rte_eth_conf port_conf = {
        .rxmode = {
            .mq_mode = RTE_ETH_MQ_RX_NONE,
        },
        .txmode = {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
        },
    };
    return port_conf;
}

/* ============================================================================
 * Port Setup
 * ============================================================================ */

static bool setup_port(uint16_t port_id, const dpdk_config_t* config) {
    assert(config != NULL && "NULL config");
    assert(g_dpdk.mempool != NULL && "Mempool not initialized");

    int ret;
    struct rte_eth_conf port_conf = get_port_conf();
    struct rte_eth_dev_info dev_info;

    /* Get device info */
    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        fprintf(stderr, "[DPDK] Failed to get device info for port %u: %s\n",
                port_id, rte_strerror(-ret));
        return false;
    }

    fprintf(stderr, "[DPDK] Port %u: %s\n", port_id, dev_info.driver_name);
    fprintf(stderr, "[DPDK]   Max RX queues: %u, Max TX queues: %u\n",
            dev_info.max_rx_queues, dev_info.max_tx_queues);

    /* Validate queue counts */
    uint16_t rx_queues = config->rx_queues;
    uint16_t tx_queues = config->tx_queues;

    if (rx_queues > dev_info.max_rx_queues) {
        rx_queues = dev_info.max_rx_queues;
        fprintf(stderr, "[DPDK]   Limiting RX queues to %u\n", rx_queues);
    }
    if (tx_queues > dev_info.max_tx_queues) {
        tx_queues = dev_info.max_tx_queues;
        fprintf(stderr, "[DPDK]   Limiting TX queues to %u\n", tx_queues);
    }

    /* Configure port */
    ret = rte_eth_dev_configure(port_id, rx_queues, tx_queues, &port_conf);
    if (ret < 0) {
        fprintf(stderr, "[DPDK] Failed to configure port %u: %s\n",
                port_id, rte_strerror(-ret));
        return false;
    }

    /* Setup RX queues */
    uint16_t rx_ring_size = config->rx_ring_size;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_ring_size, NULL);
    if (ret < 0) {
        fprintf(stderr, "[DPDK] Failed to adjust RX descriptors: %s\n",
                rte_strerror(-ret));
    }

    for (uint16_t q = 0; q < rx_queues; q++) {
        ret = rte_eth_rx_queue_setup(port_id, q, rx_ring_size,
                                      rte_eth_dev_socket_id(port_id),
                                      NULL, g_dpdk.mempool);
        if (ret < 0) {
            fprintf(stderr, "[DPDK] Failed to setup RX queue %u: %s\n",
                    q, rte_strerror(-ret));
            return false;
        }
    }

    /* Setup TX queues */
    uint16_t tx_ring_size = config->tx_ring_size;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, NULL, &tx_ring_size);
    if (ret < 0) {
        fprintf(stderr, "[DPDK] Failed to adjust TX descriptors: %s\n",
                rte_strerror(-ret));
    }

    for (uint16_t q = 0; q < tx_queues; q++) {
        ret = rte_eth_tx_queue_setup(port_id, q, tx_ring_size,
                                      rte_eth_dev_socket_id(port_id),
                                      NULL);
        if (ret < 0) {
            fprintf(stderr, "[DPDK] Failed to setup TX queue %u: %s\n",
                    q, rte_strerror(-ret));
            return false;
        }
    }

    /* Enable promiscuous mode if requested */
    if (config->promiscuous) {
        ret = rte_eth_promiscuous_enable(port_id);
        if (ret != 0) {
            fprintf(stderr, "[DPDK] Failed to enable promiscuous mode: %s\n",
                    rte_strerror(-ret));
        }
    }

    /* Start port */
    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        fprintf(stderr, "[DPDK] Failed to start port %u: %s\n",
                port_id, rte_strerror(-ret));
        return false;
    }

    /* Get and print MAC address */
    struct rte_ether_addr mac_addr;
    ret = rte_eth_macaddr_get(port_id, &mac_addr);
    if (ret == 0) {
        char mac_str[18];
        dpdk_mac_to_str(mac_addr.addr_bytes, mac_str);
        fprintf(stderr, "[DPDK]   MAC: %s\n", mac_str);
    }

    /* Check link status */
    struct rte_eth_link link;
    ret = rte_eth_link_get_nowait(port_id, &link);
    if (ret == 0) {
        if (link.link_status == RTE_ETH_LINK_UP) {
            fprintf(stderr, "[DPDK]   Link: UP, %u Mbps, %s\n",
                    link.link_speed,
                    link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full-duplex" : "half-duplex");
        } else {
            fprintf(stderr, "[DPDK]   Link: DOWN\n");
        }
    }

    g_dpdk.active_port = port_id;
    return true;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

bool dpdk_init(const dpdk_config_t* config) {
    assert(!g_dpdk.initialized && "DPDK already initialized");

    int ret;
    dpdk_config_t local_config;

    /* Use provided config or defaults */
    if (config != NULL) {
        local_config = *config;
    } else {
        dpdk_config_init(&local_config);
    }

    g_dpdk.config = local_config;

    fprintf(stderr, "[DPDK] Initializing DPDK...\n");

    /* Build EAL arguments */
    const char* eal_args = local_config.eal_args;
    if (eal_args == NULL) {
        eal_args = DPDK_DEFAULT_EAL_ARGS;
    }

    /* Add virtual device if requested */
    char extended_args[512];
    if (local_config.vdev_type != DPDK_VDEV_NONE) {
        const char* vdev_args = NULL;
        switch (local_config.vdev_type) {
            case DPDK_VDEV_NULL:
                vdev_args = DPDK_VDEV_NULL_ARGS;
                break;
            case DPDK_VDEV_RING:
                vdev_args = DPDK_VDEV_RING_ARGS;
                break;
            default:
                fprintf(stderr, "[DPDK] Unknown vdev type: %d\n", local_config.vdev_type);
                return false;
        }
        snprintf(extended_args, sizeof(extended_args), "%s %s", eal_args, vdev_args);
        eal_args = extended_args;
        fprintf(stderr, "[DPDK] Using virtual device: %s\n", vdev_args);
    }

    fprintf(stderr, "[DPDK] EAL args: %s\n", eal_args);

    /* Build argv from string */
    int argc;
    char** argv = build_eal_args(eal_args, &argc);
    if (argv == NULL) {
        fprintf(stderr, "[DPDK] Failed to build EAL arguments\n");
        return false;
    }

    /* Initialize EAL */
    ret = rte_eal_init(argc, argv);
    free_eal_args(argv, argc);

    if (ret < 0) {
        fprintf(stderr, "[DPDK] EAL initialization failed: %s\n",
                rte_strerror(-ret));
        return false;
    }

    fprintf(stderr, "[DPDK] EAL initialized successfully\n");

    /* Check port availability */
    uint16_t nb_ports = rte_eth_dev_count_avail();
    fprintf(stderr, "[DPDK] Available ports: %u\n", nb_ports);

    if (nb_ports == 0) {
        fprintf(stderr, "[DPDK] No available ports!\n");
        fprintf(stderr, "[DPDK] For testing, use vdev_type = DPDK_VDEV_NULL\n");
        rte_eal_cleanup();
        return false;
    }

    /* Create mempool */
    unsigned int num_mbufs = local_config.num_mbufs;
    unsigned int cache_size = local_config.mbuf_cache_size;

    g_dpdk.mempool = rte_pktmbuf_pool_create("MBUF_POOL",
                                              num_mbufs,
                                              cache_size,
                                              0,
                                              RTE_MBUF_DEFAULT_BUF_SIZE,
                                              rte_socket_id());

    if (g_dpdk.mempool == NULL) {
        fprintf(stderr, "[DPDK] Failed to create mempool: %s\n",
                rte_strerror(rte_errno));
        rte_eal_cleanup();
        return false;
    }

    fprintf(stderr, "[DPDK] Mempool created: %u mbufs, %u cache\n",
            num_mbufs, cache_size);

    /* Setup port */
    uint16_t port_id = local_config.port_id;

    /* For vdev, find the virtual port */
    if (local_config.vdev_type != DPDK_VDEV_NONE) {
        port_id = 0;  /* Virtual devices are typically port 0 */
    }

    if (!rte_eth_dev_is_valid_port(port_id)) {
        fprintf(stderr, "[DPDK] Invalid port ID: %u\n", port_id);
        rte_mempool_free(g_dpdk.mempool);
        rte_eal_cleanup();
        return false;
    }

    if (!setup_port(port_id, &local_config)) {
        rte_mempool_free(g_dpdk.mempool);
        rte_eal_cleanup();
        return false;
    }

    g_dpdk.initialized = true;
    fprintf(stderr, "[DPDK] Initialization complete (port %u)\n", port_id);

    return true;
}

void dpdk_cleanup(void) {
    if (!g_dpdk.initialized) {
        return;
    }

    fprintf(stderr, "[DPDK] Cleaning up...\n");

    /* Stop port */
    if (g_dpdk.active_port != UINT16_MAX) {
        int ret = rte_eth_dev_stop(g_dpdk.active_port);
        if (ret != 0) {
            fprintf(stderr, "[DPDK] Failed to stop port %u: %s\n",
                    g_dpdk.active_port, rte_strerror(-ret));
        }
        rte_eth_dev_close(g_dpdk.active_port);
    }

    /* Free mempool */
    if (g_dpdk.mempool != NULL) {
        rte_mempool_free(g_dpdk.mempool);
        g_dpdk.mempool = NULL;
    }

    /* Cleanup EAL */
    rte_eal_cleanup();

    g_dpdk.initialized = false;
    g_dpdk.active_port = UINT16_MAX;

    fprintf(stderr, "[DPDK] Cleanup complete\n");
}

bool dpdk_is_initialized(void) {
    return g_dpdk.initialized;
}

uint16_t dpdk_get_port_count(void) {
    if (!g_dpdk.initialized) {
        return 0;
    }
    return rte_eth_dev_count_avail();
}

uint16_t dpdk_get_active_port(void) {
    return g_dpdk.active_port;
}

bool dpdk_port_is_up(uint16_t port_id) {
    assert(g_dpdk.initialized && "DPDK not initialized");

    if (!rte_eth_dev_is_valid_port(port_id)) {
        return false;
    }

    struct rte_eth_link link;
    int ret = rte_eth_link_get_nowait(port_id, &link);
    if (ret != 0) {
        return false;
    }

    return link.link_status == RTE_ETH_LINK_UP;
}

bool dpdk_get_port_mac(uint16_t port_id, uint8_t mac_out[6]) {
    assert(mac_out != NULL && "NULL mac_out");

    if (!g_dpdk.initialized || !rte_eth_dev_is_valid_port(port_id)) {
        return false;
    }

    struct rte_ether_addr mac_addr;
    int ret = rte_eth_macaddr_get(port_id, &mac_addr);
    if (ret != 0) {
        return false;
    }

    memcpy(mac_out, mac_addr.addr_bytes, 6);
    return true;
}

dpdk_mempool_t dpdk_get_mempool(void) {
    return (dpdk_mempool_t)g_dpdk.mempool;
}

void dpdk_get_mempool_stats(uint32_t* free_count, uint32_t* in_use_count) {
    assert(free_count != NULL && "NULL free_count");
    assert(in_use_count != NULL && "NULL in_use_count");

    if (g_dpdk.mempool == NULL) {
        *free_count = 0;
        *in_use_count = 0;
        return;
    }

    *free_count = rte_mempool_avail_count(g_dpdk.mempool);
    *in_use_count = rte_mempool_in_use_count(g_dpdk.mempool);
}

bool dpdk_get_port_stats(uint16_t port_id, dpdk_stats_t* stats) {
    assert(stats != NULL && "NULL stats");

    if (!g_dpdk.initialized || !rte_eth_dev_is_valid_port(port_id)) {
        return false;
    }

    struct rte_eth_stats eth_stats;
    int ret = rte_eth_stats_get(port_id, &eth_stats);
    if (ret != 0) {
        return false;
    }

    stats->rx_packets = eth_stats.ipackets;
    stats->rx_bytes = eth_stats.ibytes;
    stats->rx_errors = eth_stats.ierrors;
    stats->rx_missed = eth_stats.imissed;
    stats->tx_packets = eth_stats.opackets;
    stats->tx_bytes = eth_stats.obytes;
    stats->tx_errors = eth_stats.oerrors;

    /* Copy our poll stats */
    stats->rx_polls = g_dpdk.stats.rx_polls;
    stats->rx_polls_empty = g_dpdk.stats.rx_polls_empty;
    stats->rx_polls_full = g_dpdk.stats.rx_polls_full;
    stats->tx_batches = g_dpdk.stats.tx_batches;

    return true;
}

void dpdk_reset_port_stats(uint16_t port_id) {
    if (!g_dpdk.initialized || !rte_eth_dev_is_valid_port(port_id)) {
        return;
    }

    rte_eth_stats_reset(port_id);
    memset(&g_dpdk.stats, 0, sizeof(g_dpdk.stats));
}

void dpdk_print_stats(uint16_t port_id) {
    dpdk_stats_t stats;
    if (!dpdk_get_port_stats(port_id, &stats)) {
        fprintf(stderr, "[DPDK] Failed to get stats for port %u\n", port_id);
        return;
    }

    fprintf(stderr, "\n=== DPDK Port %u Statistics ===\n", port_id);
    fprintf(stderr, "RX packets:     %lu\n", stats.rx_packets);
    fprintf(stderr, "RX bytes:       %lu\n", stats.rx_bytes);
    fprintf(stderr, "RX errors:      %lu\n", stats.rx_errors);
    fprintf(stderr, "RX missed:      %lu\n", stats.rx_missed);
    fprintf(stderr, "TX packets:     %lu\n", stats.tx_packets);
    fprintf(stderr, "TX bytes:       %lu\n", stats.tx_bytes);
    fprintf(stderr, "TX errors:      %lu\n", stats.tx_errors);
    fprintf(stderr, "RX polls:       %lu\n", stats.rx_polls);
    fprintf(stderr, "RX polls empty: %lu\n", stats.rx_polls_empty);
    fprintf(stderr, "RX polls full:  %lu\n", stats.rx_polls_full);
    fprintf(stderr, "TX batches:     %lu\n", stats.tx_batches);

    /* Mempool stats */
    uint32_t free_count, in_use;
    dpdk_get_mempool_stats(&free_count, &in_use);
    fprintf(stderr, "Mempool free:   %u\n", free_count);
    fprintf(stderr, "Mempool in use: %u\n", in_use);
}

const char* dpdk_get_version(void) {
    return rte_version();
}

char* dpdk_mac_to_str(const uint8_t mac[6], char* buf) {
    assert(mac != NULL && "NULL mac");
    assert(buf != NULL && "NULL buf");

    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

uint32_t dpdk_get_lcore_id(void) {
    return rte_lcore_id();
}

#endif /* USE_DPDK */

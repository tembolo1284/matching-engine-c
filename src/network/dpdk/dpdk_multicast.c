#ifdef USE_DPDK

#include "network/multicast_transport.h"
#include "network/dpdk/dpdk_init.h"
#include "network/dpdk/dpdk_config.h"
#include "network/transport_types.h"
#include "protocol/message_types.h"
#include "protocol/message_types_extended.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_formatter.h"
#include "threading/queues.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <time.h>

/* DPDK headers */
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

/**
 * Multicast Transport - DPDK Implementation
 *
 * Uses DPDK for high-performance multicast packet transmission.
 */

/* ============================================================================
 * Constants
 * ============================================================================ */

#define BATCH_SIZE              32
#define MAX_DRAIN_ITERATIONS    100
#define MAX_OUTPUT_QUEUES       2

static const struct timespec idle_sleep = {
    .tv_sec = 0,
    .tv_nsec = 1000
};

/* ============================================================================
 * Transport Structure
 * ============================================================================ */

struct multicast_transport {
    multicast_transport_config_t config;
    
    /* DPDK port/queue */
    uint16_t port_id;
    uint16_t tx_queue;
    
    /* Multicast destination */
    struct rte_ether_addr mcast_mac;
    uint32_t mcast_ip;
    uint16_t mcast_port;
    
    /* Output queues */
    output_envelope_queue_t* output_queues[MAX_OUTPUT_QUEUES];
    int num_queues;
    
    /* Threading */
    pthread_t publisher_thread;
    atomic_bool* shutdown_flag;
    atomic_bool running;
    atomic_bool started;
    
    /* Formatters */
    message_formatter_t csv_formatter;
    binary_message_formatter_t binary_formatter;
    
    /* Statistics */
    multicast_transport_stats_t stats;
};

/* ============================================================================
 * Address Validation
 * ============================================================================ */

bool multicast_address_is_valid(const char* addr) {
    if (addr == NULL || addr[0] == '\0') {
        return false;
    }
    
    /* Parse IP */
    uint32_t a, b, c, d;
    if (sscanf(addr, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    
    /* Multicast: 224.0.0.0 - 239.255.255.255 */
    return (a >= 224 && a <= 239);
}

/* ============================================================================
 * Packet Building
 * ============================================================================ */

static struct rte_mbuf* build_udp_packet(multicast_transport_t* t,
                                          const void* payload,
                                          size_t payload_len) {
    assert(t != NULL && "NULL transport");
    assert(payload != NULL && "NULL payload");
    
    struct rte_mempool* mp = dpdk_get_mempool();
    if (mp == NULL) {
        return NULL;
    }
    
    struct rte_mbuf* m = rte_pktmbuf_alloc(mp);
    if (m == NULL) {
        return NULL;
    }
    
    size_t total_len = DPDK_HEADER_OVERHEAD + payload_len;
    
    if (total_len > rte_pktmbuf_tailroom(m)) {
        rte_pktmbuf_free(m);
        return NULL;
    }
    
    char* pkt = rte_pktmbuf_mtod(m, char*);
    
    /* Ethernet header */
    struct rte_ether_hdr* eth = (struct rte_ether_hdr*)pkt;
    memcpy(&eth->dst_addr, &t->mcast_mac, sizeof(struct rte_ether_addr));
    memset(&eth->src_addr, 0, sizeof(struct rte_ether_addr));  /* Will be filled by NIC */
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    
    /* IP header */
    struct rte_ipv4_hdr* ip = (struct rte_ipv4_hdr*)(eth + 1);
    ip->version_ihl = 0x45;
    ip->type_of_service = 0;
    ip->total_length = rte_cpu_to_be_16(DPDK_IPV4_HDR_SIZE + DPDK_UDP_HDR_SIZE + payload_len);
    ip->packet_id = 0;
    ip->fragment_offset = 0;
    ip->time_to_live = t->config.ttl;
    ip->next_proto_id = IPPROTO_UDP;
    ip->hdr_checksum = 0;
    ip->src_addr = 0;  /* Will be filled */
    ip->dst_addr = t->mcast_ip;
    
    /* UDP header */
    struct rte_udp_hdr* udp = (struct rte_udp_hdr*)(ip + 1);
    udp->src_port = rte_cpu_to_be_16(t->mcast_port);
    udp->dst_port = rte_cpu_to_be_16(t->mcast_port);
    udp->dgram_len = rte_cpu_to_be_16(DPDK_UDP_HDR_SIZE + payload_len);
    udp->dgram_cksum = 0;
    
    /* Payload */
    memcpy((uint8_t*)(udp + 1), payload, payload_len);
    
    m->data_len = total_len;
    m->pkt_len = total_len;
    
    /* Offload checksum to hardware if available */
    m->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
    m->l2_len = DPDK_ETHER_HDR_SIZE;
    m->l3_len = DPDK_IPV4_HDR_SIZE;
    
    return m;
}

/* ============================================================================
 * Message Sending
 * ============================================================================ */

static bool send_message_internal(multicast_transport_t* t, const output_msg_t* msg) {
    assert(t != NULL && "NULL transport");
    assert(msg != NULL && "NULL msg");
    
    const void* data = NULL;
    size_t len = 0;
    char csv_buf[1024];
    
    if (t->config.use_binary) {
        data = binary_message_formatter_format(&t->binary_formatter, msg, &len);
        if (data == NULL || len == 0) {
            t->stats.format_errors++;
            return false;
        }
    } else {
        const char* text = message_formatter_format(&t->csv_formatter, msg);
        if (text == NULL) {
            t->stats.format_errors++;
            return false;
        }
        strncpy(csv_buf, text, sizeof(csv_buf) - 1);
        csv_buf[sizeof(csv_buf) - 1] = '\0';
        data = csv_buf;
        len = strlen(csv_buf);
    }
    
    struct rte_mbuf* m = build_udp_packet(t, data, len);
    if (m == NULL) {
        t->stats.tx_errors++;
        return false;
    }
    
    uint16_t nb_tx = rte_eth_tx_burst(t->port_id, t->tx_queue, &m, 1);
    
    if (nb_tx == 0) {
        rte_pktmbuf_free(m);
        t->stats.tx_errors++;
        return false;
    }
    
    t->stats.tx_packets++;
    t->stats.tx_bytes += len;
    t->stats.tx_messages++;
    t->stats.sequence++;
    
    return true;
}

/* ============================================================================
 * Publisher Thread
 * ============================================================================ */

static void* publisher_thread_func(void* arg) {
    multicast_transport_t* t = (multicast_transport_t*)arg;
    
    assert(t != NULL && "NULL transport");
    
    fprintf(stderr, "[DPDK-Multicast] Publisher started (%s:%u)\n",
            t->config.group_addr, t->config.port);
    
    message_formatter_init(&t->csv_formatter);
    binary_message_formatter_init(&t->binary_formatter);
    
    output_msg_envelope_t batch[BATCH_SIZE];
    
    while (!atomic_load(t->shutdown_flag)) {
        size_t total_processed = 0;
        
        for (int q = 0; q < t->num_queues; q++) {
            output_envelope_queue_t* queue = t->output_queues[q];
            if (queue == NULL) continue;
            
            size_t count = 0;
            for (size_t i = 0; i < BATCH_SIZE; i++) {
                if (output_envelope_queue_dequeue(queue, &batch[count])) {
                    count++;
                } else {
                    break;
                }
            }
            
            for (size_t i = 0; i < count; i++) {
                if (send_message_internal(t, &batch[i].msg)) {
                    if (q == 0) {
                        t->stats.messages_from_queue_0++;
                    } else {
                        t->stats.messages_from_queue_1++;
                    }
                }
            }
            
            total_processed += count;
        }
        
        if (total_processed == 0) {
            nanosleep(&idle_sleep, NULL);
        }
    }
    
    /* Drain on shutdown */
    for (int iter = 0; iter < MAX_DRAIN_ITERATIONS; iter++) {
        bool has_messages = false;
        
        for (int q = 0; q < t->num_queues; q++) {
            output_envelope_queue_t* queue = t->output_queues[q];
            if (queue == NULL) continue;
            
            output_msg_envelope_t envelope;
            while (output_envelope_queue_dequeue(queue, &envelope)) {
                has_messages = true;
                send_message_internal(t, &envelope.msg);
            }
        }
        
        if (!has_messages) break;
    }
    
    fprintf(stderr, "[DPDK-Multicast] Publisher stopped\n");
    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

multicast_transport_t* multicast_transport_create(
    const multicast_transport_config_t* config,
    output_envelope_queue_t* output_queue_0,
    output_envelope_queue_t* output_queue_1,
    atomic_bool* shutdown_flag) {
    
    assert(config != NULL && "NULL config");
    assert(config->group_addr != NULL && "NULL group_addr");
    assert(output_queue_0 != NULL && "NULL output_queue_0");
    assert(shutdown_flag != NULL && "NULL shutdown_flag");
    
    if (!dpdk_is_initialized()) {
        fprintf(stderr, "[DPDK-Multicast] DPDK not initialized\n");
        return NULL;
    }
    
    if (!multicast_address_is_valid(config->group_addr)) {
        fprintf(stderr, "[DPDK-Multicast] Invalid multicast address: %s\n",
                config->group_addr);
        return NULL;
    }
    
    multicast_transport_t* t = calloc(1, sizeof(multicast_transport_t));
    if (t == NULL) {
        return NULL;
    }
    
    t->config = *config;
    t->port_id = dpdk_get_port_id();
    t->tx_queue = 0;
    t->output_queues[0] = output_queue_0;
    t->output_queues[1] = output_queue_1;
    t->num_queues = (output_queue_1 != NULL) ? 2 : 1;
    t->shutdown_flag = shutdown_flag;
    
    /* Parse multicast IP */
    uint32_t a, b, c, d;
    sscanf(config->group_addr, "%u.%u.%u.%u", &a, &b, &c, &d);
    t->mcast_ip = rte_cpu_to_be_32((a << 24) | (b << 16) | (c << 8) | d);
    t->mcast_port = config->port;
    
    /* Calculate multicast MAC: 01:00:5e:xx:xx:xx */
    uint64_t mac = dpdk_mcast_ip_to_mac(t->mcast_ip);
    t->mcast_mac.addr_bytes[0] = 0x01;
    t->mcast_mac.addr_bytes[1] = 0x00;
    t->mcast_mac.addr_bytes[2] = 0x5e;
    t->mcast_mac.addr_bytes[3] = (mac >> 16) & 0x7f;
    t->mcast_mac.addr_bytes[4] = (mac >> 8) & 0xff;
    t->mcast_mac.addr_bytes[5] = mac & 0xff;
    
    atomic_init(&t->running, false);
    atomic_init(&t->started, false);
    memset(&t->stats, 0, sizeof(t->stats));
    
    fprintf(stderr, "[DPDK-Multicast] Created (%s:%u)\n",
            config->group_addr, config->port);
    
    return t;
}

bool multicast_transport_start(multicast_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    
    bool expected = false;
    if (!atomic_compare_exchange_strong(&transport->started, &expected, true)) {
        return false;
    }
    
    atomic_store(&transport->running, true);
    
    int rc = pthread_create(&transport->publisher_thread, NULL,
                            publisher_thread_func, transport);
    if (rc != 0) {
        atomic_store(&transport->running, false);
        atomic_store(&transport->started, false);
        return false;
    }
    
    return true;
}

void multicast_transport_stop(multicast_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    
    if (!atomic_load(&transport->started)) {
        return;
    }
    
    atomic_store(&transport->running, false);
    pthread_join(transport->publisher_thread, NULL);
    atomic_store(&transport->started, false);
    
    multicast_transport_print_stats(transport);
}

void multicast_transport_destroy(multicast_transport_t* transport) {
    if (transport == NULL) return;
    
    if (atomic_load(&transport->started)) {
        multicast_transport_stop(transport);
    }
    
    free(transport);
}

bool multicast_transport_send(multicast_transport_t* transport,
                               const void* data,
                               size_t len) {
    assert(transport != NULL && "NULL transport");
    assert(data != NULL && "NULL data");
    
    struct rte_mbuf* m = build_udp_packet(transport, data, len);
    if (m == NULL) {
        transport->stats.tx_errors++;
        return false;
    }
    
    uint16_t nb_tx = rte_eth_tx_burst(transport->port_id, transport->tx_queue, &m, 1);
    
    if (nb_tx == 0) {
        rte_pktmbuf_free(m);
        transport->stats.tx_errors++;
        return false;
    }
    
    transport->stats.tx_packets++;
    transport->stats.tx_bytes += len;
    
    return true;
}

bool multicast_transport_send_message(multicast_transport_t* transport,
                                       const output_msg_t* msg) {
    assert(transport != NULL && "NULL transport");
    assert(msg != NULL && "NULL msg");
    
    return send_message_internal(transport, msg);
}

void multicast_transport_get_stats(const multicast_transport_t* transport,
                                    multicast_transport_stats_t* stats) {
    assert(transport != NULL && "NULL transport");
    assert(stats != NULL && "NULL stats");
    
    *stats = transport->stats;
}

void multicast_transport_reset_stats(multicast_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    
    transport->stats.tx_packets = 0;
    transport->stats.tx_bytes = 0;
    transport->stats.tx_messages = 0;
    transport->stats.tx_errors = 0;
    transport->stats.messages_from_queue_0 = 0;
    transport->stats.messages_from_queue_1 = 0;
    transport->stats.format_errors = 0;
}

void multicast_transport_print_stats(const multicast_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    
    fprintf(stderr, "\n=== DPDK Multicast Statistics ===\n");
    fprintf(stderr, "Backend:        DPDK\n");
    fprintf(stderr, "Group:          %s:%u\n",
            transport->config.group_addr, transport->config.port);
    fprintf(stderr, "TX packets:     %lu\n", transport->stats.tx_packets);
    fprintf(stderr, "TX bytes:       %lu\n", transport->stats.tx_bytes);
    fprintf(stderr, "TX messages:    %lu\n", transport->stats.tx_messages);
    fprintf(stderr, "TX errors:      %lu\n", transport->stats.tx_errors);
    fprintf(stderr, "From queue 0:   %lu\n", transport->stats.messages_from_queue_0);
    fprintf(stderr, "From queue 1:   %lu\n", transport->stats.messages_from_queue_1);
    fprintf(stderr, "Sequence:       %lu\n", transport->stats.sequence);
}

bool multicast_transport_is_running(const multicast_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    return atomic_load(&transport->running);
}

uint64_t multicast_transport_get_sequence(const multicast_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    return transport->stats.sequence;
}

const char* multicast_transport_get_backend(void) {
    return "dpdk";
}

#endif /* USE_DPDK */

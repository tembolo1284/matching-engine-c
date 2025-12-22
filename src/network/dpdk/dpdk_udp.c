#ifdef USE_DPDK

#include "network/udp_transport.h"
#include "network/dpdk/dpdk_init.h"
#include "network/dpdk/dpdk_config.h"
#include "network/transport_types.h"
#include "protocol/message_types.h"
#include "protocol/message_types_extended.h"
#include "protocol/csv/message_parser.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/symbol_router.h"
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
#include <rte_lcore.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

/**
 * UDP Transport - DPDK Implementation
 *
 * This implementation uses DPDK for ultra-low-latency packet I/O.
 * It implements the same udp_transport API as the socket version.
 */

/* ============================================================================
 * Constants
 * ============================================================================ */

#define BURST_SIZE          DPDK_RX_BURST_SIZE
#define MAX_CLIENTS         TRANSPORT_MAX_CLIENTS
#define CLIENT_HASH_SIZE    TRANSPORT_CLIENT_HASH_SIZE
#define CLIENT_HASH_MASK    (CLIENT_HASH_SIZE - 1)
#define MAX_PROBE_LENGTH    128

_Static_assert((CLIENT_HASH_SIZE & CLIENT_HASH_MASK) == 0,
               "CLIENT_HASH_SIZE must be power of 2");

/* ============================================================================
 * Client Entry
 * ============================================================================ */

typedef struct {
    int64_t last_seen;
    transport_addr_t addr;
    uint32_t client_id;
    transport_protocol_t protocol;
    bool active;
} client_entry_t;

/* ============================================================================
 * Transport Structure
 * ============================================================================ */

struct udp_transport {
    /* Configuration */
    udp_transport_config_t config;
    uint16_t port_id;
    uint16_t rx_queue;
    uint16_t tx_queue;

    /* Filter settings */
    uint16_t filter_port;
    uint32_t filter_ip;

    /* Queues */
    input_envelope_queue_t* input_queue_0;
    input_envelope_queue_t* input_queue_1;

    /* Threading */
    pthread_t rx_thread;
    atomic_bool* shutdown_flag;
    atomic_bool running;
    atomic_bool started;

    /* Client tracking */
    client_entry_t clients[CLIENT_HASH_SIZE];
    uint32_t next_client_id;
    atomic_uint_fast32_t active_clients;
    uint32_t peak_clients;

    /* Last received address */
    transport_addr_t last_recv_addr;
    bool has_last_recv;

    /* Statistics */
    transport_stats_t stats;
    
    /* Sequence number */
    uint64_t sequence;

    /* Thread-local parsers */
    message_parser_t csv_parser;
    binary_message_parser_t binary_parser;
};

/* ============================================================================
 * Client Hash Table Operations
 * ============================================================================ */

static uint32_t hash_addr(const transport_addr_t* addr) {
    assert(addr != NULL && "NULL addr");
    return transport_addr_hash(addr);
}

static client_entry_t* find_client_by_id(udp_transport_t* t, uint32_t client_id) {
    assert(t != NULL && "NULL transport");
    
    for (uint32_t i = 0; i < CLIENT_HASH_SIZE; i++) {
        client_entry_t* entry = &t->clients[i];
        if (entry->active && entry->client_id == client_id) {
            return entry;
        }
    }
    return NULL;
}

static client_entry_t* add_or_update_client(udp_transport_t* t,
                                             const transport_addr_t* addr,
                                             transport_protocol_t protocol) {
    assert(t != NULL && "NULL transport");
    assert(addr != NULL && "NULL addr");
    
    uint32_t hash = hash_addr(addr);
    uint32_t index = hash & CLIENT_HASH_MASK;
    client_entry_t* empty_slot = NULL;
    
    for (uint32_t probe = 0; probe < MAX_PROBE_LENGTH; probe++) {
        client_entry_t* entry = &t->clients[index];
        
        if (!entry->active) {
            if (empty_slot == NULL) {
                empty_slot = entry;
            }
        } else if (transport_addr_equal(&entry->addr, addr)) {
            entry->last_seen = (int64_t)time(NULL);
            if (protocol != TRANSPORT_PROTO_UNKNOWN) {
                entry->protocol = protocol;
            }
            return entry;
        }
        
        index = (index + 1) & CLIENT_HASH_MASK;
    }
    
    if (empty_slot != NULL) {
        empty_slot->addr = *addr;
        empty_slot->client_id = t->next_client_id++;
        empty_slot->protocol = protocol;
        empty_slot->last_seen = (int64_t)time(NULL);
        empty_slot->active = true;
        
        uint32_t count = atomic_fetch_add(&t->active_clients, 1) + 1;
        if (count > t->peak_clients) {
            t->peak_clients = count;
        }
        
        return empty_slot;
    }
    
    return NULL;
}

/* ============================================================================
 * Protocol Detection
 * ============================================================================ */

static transport_protocol_t detect_protocol(const uint8_t* data, size_t len) {
    assert(data != NULL && "NULL data");
    
    if (len < 2) {
        return TRANSPORT_PROTO_UNKNOWN;
    }
    
    if (data[0] == 0x4D) {
        return TRANSPORT_PROTO_BINARY;
    }
    
    if ((data[0] >= 'A' && data[0] <= 'Z') ||
        (data[0] >= 'a' && data[0] <= 'z')) {
        return TRANSPORT_PROTO_CSV;
    }
    
    return TRANSPORT_PROTO_UNKNOWN;
}

/* ============================================================================
 * Packet Processing
 * ============================================================================ */

static void process_udp_packet(udp_transport_t* t,
                                const uint8_t* payload,
                                size_t payload_len,
                                uint32_t src_ip,
                                uint16_t src_port) {
    assert(t != NULL && "NULL transport");
    assert(payload != NULL && "NULL payload");
    
    t->stats.rx_packets++;
    t->stats.rx_bytes += payload_len;
    
    /* Build source address */
    transport_addr_t src;
    src.ip_addr = src_ip;
    src.port = src_port;
    src._pad = 0;
    
    t->last_recv_addr = src;
    t->has_last_recv = true;
    
    /* Detect protocol */
    transport_protocol_t protocol = t->config.default_protocol;
    if (t->config.detect_protocol) {
        transport_protocol_t detected = detect_protocol(payload, payload_len);
        if (detected != TRANSPORT_PROTO_UNKNOWN) {
            protocol = detected;
        }
    }
    
    /* Find or create client */
    client_entry_t* client = add_or_update_client(t, &src, protocol);
    uint32_t client_id = client ? client->client_id : 0;
    
    /* Parse message */
    input_msg_t msg;
    bool parsed = false;
    
    if (protocol == TRANSPORT_PROTO_BINARY) {
        parsed = binary_message_parser_parse(&t->binary_parser, payload, payload_len, &msg);
    } else {
        /* CSV - need to null-terminate */
        char csv_buf[2048];
        size_t copy_len = payload_len < sizeof(csv_buf) - 1 ? payload_len : sizeof(csv_buf) - 1;
        memcpy(csv_buf, payload, copy_len);
        csv_buf[copy_len] = '\0';
        parsed = message_parser_parse(&t->csv_parser, csv_buf, &msg);
    }
    
    if (!parsed) {
        t->stats.rx_errors++;
        return;
    }
    
    t->stats.rx_messages++;
    
    /* Create envelope using project's helper */
    udp_client_addr_t udp_addr;
    udp_addr.addr = src_ip;
    udp_addr.port = src_port;
    udp_addr._pad = 0;
    
    uint64_t seq = t->sequence++;
    input_msg_envelope_t envelope = create_input_envelope_udp(&msg, client_id, &udp_addr, seq);
    
    /* Route to appropriate queue */
    input_envelope_queue_t* target = t->input_queue_0;
    
    if (t->config.dual_processor && t->input_queue_1 != NULL) {
        const char* symbol = NULL;
        
        if (msg.type == INPUT_MSG_NEW_ORDER) {
            symbol = msg.data.new_order.symbol;
        } else if (msg.type == INPUT_MSG_CANCEL) {
            symbol = msg.data.cancel.symbol;
        }
        
        if (symbol != NULL) {
            int processor_id = get_processor_id_for_symbol(symbol);
            target = (processor_id == 0) ? t->input_queue_0 : t->input_queue_1;
        }
        
        /* Flush goes to both queues */
        if (msg.type == INPUT_MSG_FLUSH) {
            input_envelope_queue_enqueue(t->input_queue_0, &envelope);
            input_envelope_queue_enqueue(t->input_queue_1, &envelope);
            return;
        }
    }
    
    if (!input_envelope_queue_enqueue(target, &envelope)) {
        t->stats.rx_dropped++;
    }
}

/* ============================================================================
 * RX Thread
 * ============================================================================ */

static void* rx_thread_func(void* arg) {
    udp_transport_t* t = (udp_transport_t*)arg;
    
    assert(t != NULL && "NULL transport");
    
    fprintf(stderr, "[DPDK-UDP] RX thread started (port %u, queue %u)\n",
            t->port_id, t->rx_queue);
    
    message_parser_init(&t->csv_parser);
    binary_message_parser_init(&t->binary_parser);
    
    struct rte_mbuf* rx_pkts[BURST_SIZE];
    
    while (!atomic_load(t->shutdown_flag)) {
        uint16_t nb_rx = rte_eth_rx_burst(t->port_id, t->rx_queue,
                                           rx_pkts, BURST_SIZE);
        
        if (nb_rx == 0) {
            continue;
        }
        
        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf* m = rx_pkts[i];
            
            /* Get Ethernet header */
            struct rte_ether_hdr* eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
            
            /* Only process IPv4 */
            if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                rte_pktmbuf_free(m);
                continue;
            }
            
            /* Get IP header */
            struct rte_ipv4_hdr* ip = (struct rte_ipv4_hdr*)(eth + 1);
            
            /* Only process UDP */
            if (ip->next_proto_id != IPPROTO_UDP) {
                rte_pktmbuf_free(m);
                continue;
            }
            
            /* Get UDP header */
            struct rte_udp_hdr* udp = (struct rte_udp_hdr*)((uint8_t*)ip + 
                                       (ip->version_ihl & 0x0f) * 4);
            
            /* Filter by port if configured */
            if (t->filter_port != 0 && 
                rte_be_to_cpu_16(udp->dst_port) != t->filter_port) {
                rte_pktmbuf_free(m);
                continue;
            }
            
            /* Get UDP payload */
            uint8_t* payload = (uint8_t*)(udp + 1);
            size_t payload_len = rte_be_to_cpu_16(udp->dgram_len) - sizeof(struct rte_udp_hdr);
            
            /* Process the packet */
            process_udp_packet(t, payload, payload_len, ip->src_addr, udp->src_port);
            
            rte_pktmbuf_free(m);
        }
    }
    
    fprintf(stderr, "[DPDK-UDP] RX thread stopped\n");
    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

udp_transport_t* udp_transport_create(const udp_transport_config_t* config,
                                       input_envelope_queue_t* input_queue_0,
                                       input_envelope_queue_t* input_queue_1,
                                       atomic_bool* shutdown_flag) {
    assert(config != NULL && "NULL config");
    assert(input_queue_0 != NULL && "NULL input_queue_0");
    assert(shutdown_flag != NULL && "NULL shutdown_flag");
    
    if (!dpdk_is_initialized()) {
        fprintf(stderr, "[DPDK-UDP] DPDK not initialized\n");
        return NULL;
    }
    
    udp_transport_t* t = calloc(1, sizeof(udp_transport_t));
    if (t == NULL) {
        fprintf(stderr, "[DPDK-UDP] Failed to allocate transport\n");
        return NULL;
    }
    
    t->config = *config;
    t->port_id = dpdk_get_port_id();
    t->rx_queue = 0;
    t->tx_queue = 0;
    t->filter_port = config->bind_port;
    t->filter_ip = 0;
    t->input_queue_0 = input_queue_0;
    t->input_queue_1 = input_queue_1;
    t->shutdown_flag = shutdown_flag;
    t->next_client_id = CLIENT_ID_UDP_BASE + 1;
    t->sequence = 0;
    
    atomic_init(&t->running, false);
    atomic_init(&t->started, false);
    atomic_init(&t->active_clients, 0);
    
    memset(&t->stats, 0, sizeof(t->stats));
    memset(t->clients, 0, sizeof(t->clients));
    
    fprintf(stderr, "[DPDK-UDP] Created transport (port %u, filter UDP %u)\n",
            t->port_id, t->filter_port);
    
    return t;
}

bool udp_transport_start(udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    
    bool expected = false;
    if (!atomic_compare_exchange_strong(&transport->started, &expected, true)) {
        fprintf(stderr, "[DPDK-UDP] Already started\n");
        return false;
    }
    
    atomic_store(&transport->running, true);
    
    int rc = pthread_create(&transport->rx_thread, NULL, rx_thread_func, transport);
    if (rc != 0) {
        fprintf(stderr, "[DPDK-UDP] pthread_create failed: %d\n", rc);
        atomic_store(&transport->running, false);
        atomic_store(&transport->started, false);
        return false;
    }
    
    return true;
}

void udp_transport_stop(udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    
    if (!atomic_load(&transport->started)) {
        return;
    }
    
    atomic_store(&transport->running, false);
    pthread_join(transport->rx_thread, NULL);
    atomic_store(&transport->started, false);
    
    udp_transport_print_stats(transport);
}

void udp_transport_destroy(udp_transport_t* transport) {
    if (transport == NULL) {
        return;
    }
    
    if (atomic_load(&transport->started)) {
        udp_transport_stop(transport);
    }
    
    free(transport);
}

bool udp_transport_send_to_client(udp_transport_t* transport,
                                   uint32_t client_id,
                                   const void* data,
                                   size_t len) {
    assert(transport != NULL && "NULL transport");
    assert(data != NULL && "NULL data");
    
    client_entry_t* client = find_client_by_id(transport, client_id);
    if (client == NULL) {
        return false;
    }
    
    return udp_transport_send_to_addr(transport, &client->addr, data, len);
}

bool udp_transport_send_to_addr(udp_transport_t* transport,
                                 const transport_addr_t* addr,
                                 const void* data,
                                 size_t len) {
    assert(transport != NULL && "NULL transport");
    assert(addr != NULL && "NULL addr");
    assert(data != NULL && "NULL data");
    
    /* Get mempool and allocate mbuf */
    struct rte_mempool* mp = dpdk_get_mempool();
    if (mp == NULL) {
        transport->stats.tx_errors++;
        return false;
    }
    
    struct rte_mbuf* m = rte_pktmbuf_alloc(mp);
    if (m == NULL) {
        transport->stats.tx_errors++;
        return false;
    }
    
    /* Build packet - simplified for now */
    /* In production, would need proper IP/UDP header construction */
    
    /* For now, just copy data and send */
    char* pkt_data = rte_pktmbuf_mtod(m, char*);
    size_t hdr_len = DPDK_HEADER_OVERHEAD;
    
    if (len + hdr_len > rte_pktmbuf_tailroom(m)) {
        rte_pktmbuf_free(m);
        transport->stats.tx_errors++;
        return false;
    }
    
    /* Copy payload after headers */
    memcpy(pkt_data + hdr_len, data, len);
    m->data_len = hdr_len + len;
    m->pkt_len = m->data_len;
    
    /* Send */
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

bool udp_transport_send_to_last(udp_transport_t* transport,
                                 const void* data,
                                 size_t len) {
    assert(transport != NULL && "NULL transport");
    assert(data != NULL && "NULL data");
    
    if (!transport->has_last_recv) {
        return false;
    }
    
    return udp_transport_send_to_addr(transport, &transport->last_recv_addr, data, len);
}

size_t udp_transport_broadcast(udp_transport_t* transport,
                                const void* data,
                                size_t len) {
    assert(transport != NULL && "NULL transport");
    assert(data != NULL && "NULL data");
    
    size_t sent_count = 0;
    
    for (uint32_t i = 0; i < CLIENT_HASH_SIZE; i++) {
        client_entry_t* client = &transport->clients[i];
        if (client->active) {
            if (udp_transport_send_to_addr(transport, &client->addr, data, len)) {
                sent_count++;
            }
        }
    }
    
    return sent_count;
}

bool udp_transport_get_client_addr(const udp_transport_t* transport,
                                    uint32_t client_id,
                                    transport_addr_t* addr) {
    assert(transport != NULL && "NULL transport");
    assert(addr != NULL && "NULL addr");
    
    client_entry_t* client = find_client_by_id((udp_transport_t*)transport, client_id);
    if (client == NULL) {
        return false;
    }
    
    *addr = client->addr;
    return true;
}

transport_protocol_t udp_transport_get_client_protocol(const udp_transport_t* transport,
                                                        uint32_t client_id) {
    assert(transport != NULL && "NULL transport");
    
    client_entry_t* client = find_client_by_id((udp_transport_t*)transport, client_id);
    if (client == NULL) {
        return TRANSPORT_PROTO_UNKNOWN;
    }
    
    return client->protocol;
}

size_t udp_transport_evict_inactive(udp_transport_t* transport, uint32_t timeout_sec) {
    assert(transport != NULL && "NULL transport");
    
    int64_t now = (int64_t)time(NULL);
    int64_t cutoff = now - (int64_t)timeout_sec;
    size_t evicted = 0;
    
    for (uint32_t i = 0; i < CLIENT_HASH_SIZE; i++) {
        client_entry_t* client = &transport->clients[i];
        if (client->active && client->last_seen < cutoff) {
            client->active = false;
            atomic_fetch_sub(&transport->active_clients, 1);
            evicted++;
        }
    }
    
    return evicted;
}

void udp_transport_get_stats(const udp_transport_t* transport, transport_stats_t* stats) {
    assert(transport != NULL && "NULL transport");
    assert(stats != NULL && "NULL stats");
    
    *stats = transport->stats;
    stats->active_clients = atomic_load(&transport->active_clients);
    stats->peak_clients = transport->peak_clients;
}

void udp_transport_reset_stats(udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    transport_stats_reset(&transport->stats);
}

void udp_transport_print_stats(const udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    
    transport_stats_t stats;
    udp_transport_get_stats(transport, &stats);
    
    fprintf(stderr, "\n=== DPDK UDP Transport Statistics ===\n");
    fprintf(stderr, "Backend:        DPDK\n");
    fprintf(stderr, "RX packets:     %lu\n", stats.rx_packets);
    fprintf(stderr, "RX bytes:       %lu\n", stats.rx_bytes);
    fprintf(stderr, "RX messages:    %lu\n", stats.rx_messages);
    fprintf(stderr, "RX errors:      %lu\n", stats.rx_errors);
    fprintf(stderr, "RX dropped:     %lu\n", stats.rx_dropped);
    fprintf(stderr, "TX packets:     %lu\n", stats.tx_packets);
    fprintf(stderr, "TX bytes:       %lu\n", stats.tx_bytes);
    fprintf(stderr, "TX errors:      %lu\n", stats.tx_errors);
    fprintf(stderr, "Active clients: %u\n", stats.active_clients);
    fprintf(stderr, "Peak clients:   %u\n", stats.peak_clients);
}

bool udp_transport_is_running(const udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    return atomic_load(&transport->running);
}

uint16_t udp_transport_get_port(const udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    return transport->filter_port;
}

const char* udp_transport_get_backend(void) {
    return "dpdk";
}

#endif /* USE_DPDK */

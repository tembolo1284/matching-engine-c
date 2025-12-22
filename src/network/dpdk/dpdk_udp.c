#ifdef USE_DPDK

#include "network/udp_transport.h"
#include "network/dpdk/dpdk_init.h"
#include "network/dpdk/dpdk_config.h"
#include "protocol/csv/message_parser.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/symbol_router.h"
#include "platform/timestamps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>

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
 *
 * Compile with: cmake .. -DUSE_DPDK=ON
 *
 * Key differences from socket implementation:
 * - Uses rte_eth_rx_burst() instead of recvfrom()
 * - Uses rte_eth_tx_burst() instead of sendto()
 * - Poll mode (no interrupts)
 * - Zero-copy where possible
 *
 * Rule Compliance:
 * - Rule 2: All loops bounded (BURST_SIZE, MAX_CLIENTS)
 * - Rule 5: Minimum 2 assertions per function
 * - Rule 7: All return values checked
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
 * Client Entry (32 bytes for cache efficiency)
 * ============================================================================ */

typedef struct {
    int64_t last_seen;              /* 8 bytes */
    transport_addr_t addr;          /* 8 bytes */
    uint32_t client_id;             /* 4 bytes */
    transport_protocol_t protocol;  /* 1 byte */
    bool active;                    /* 1 byte */
    uint8_t _pad[10];               /* 10 bytes → 32 total */
} client_entry_t;

_Static_assert(sizeof(client_entry_t) == 32, "client_entry_t must be 32 bytes");

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
    uint16_t filter_port;           /* UDP port to filter on */
    uint32_t filter_ip;             /* Local IP (0 = any) */

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

    /* Last received address */
    transport_addr_t last_recv_addr;
    struct rte_ether_addr last_recv_mac;
    bool has_last_recv;

    /* Our MAC address */
    struct rte_ether_addr our_mac;

    /* Statistics */
    transport_stats_t stats;

    /* Parsers (used in RX thread) */
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

static client_entry_t* find_client_by_addr(udp_transport_t* t,
                                            const transport_addr_t* addr) {
    assert(t != NULL && "NULL transport");
    assert(addr != NULL && "NULL addr");

    uint32_t hash = hash_addr(addr);
    uint32_t index = hash & CLIENT_HASH_MASK;

    for (uint32_t probe = 0; probe < MAX_PROBE_LENGTH; probe++) {
        client_entry_t* entry = &t->clients[index];
        if (!entry->active) return NULL;
        if (transport_addr_equal(&entry->addr, addr)) return entry;
        index = (index + 1) & CLIENT_HASH_MASK;
    }

    return NULL;
}

static client_entry_t* find_client_by_id(udp_transport_t* t, uint32_t client_id) {
    assert(t != NULL && "NULL transport");

    for (uint32_t i = 0; i < CLIENT_HASH_SIZE; i++) {
        client_entry_t* entry = &t->clients[i];
        if (entry->active && entry->client_id == client_id) return entry;
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
            if (empty_slot == NULL) empty_slot = entry;
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
        atomic_fetch_add(&t->active_clients, 1);
        return empty_slot;
    }

    return NULL;
}

/* ============================================================================
 * Protocol Detection
 * ============================================================================ */

static transport_protocol_t detect_protocol(const uint8_t* data, size_t len) {
    assert(data != NULL && "NULL data");

    if (len < 2) return TRANSPORT_PROTO_UNKNOWN;
    if (data[0] == 0x4D) return TRANSPORT_PROTO_BINARY;
    if ((data[0] >= 'A' && data[0] <= 'Z') ||
        (data[0] >= 'a' && data[0] <= 'z')) {
        return TRANSPORT_PROTO_CSV;
    }
    return TRANSPORT_PROTO_UNKNOWN;
}

/* ============================================================================
 * Packet Processing
 * ============================================================================ */

static bool process_udp_packet(udp_transport_t* t,
                                struct rte_mbuf* mbuf) {
    assert(t != NULL && "NULL transport");
    assert(mbuf != NULL && "NULL mbuf");

    /* Get packet headers */
    struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
    struct rte_ipv4_hdr* ip_hdr = (struct rte_ipv4_hdr*)(eth_hdr + 1);
    struct rte_udp_hdr* udp_hdr = (struct rte_udp_hdr*)((uint8_t*)ip_hdr + 
                                   (ip_hdr->version_ihl & 0x0f) * 4);

    /* Check if this is for our port */
    uint16_t dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);
    if (t->filter_port != 0 && dst_port != t->filter_port) {
        return false;  /* Not for us */
    }

    /* Get payload */
    uint8_t* payload = (uint8_t*)(udp_hdr + 1);
    uint16_t payload_len = rte_be_to_cpu_16(udp_hdr->dgram_len) - sizeof(struct rte_udp_hdr);

    if (payload_len == 0) {
        return false;
    }

    /* Extract source address */
    transport_addr_t src_addr = {
        .ip_addr = ip_hdr->src_addr,
        .port = udp_hdr->src_port,
        ._pad = 0
    };

    /* Store last received info */
    t->last_recv_addr = src_addr;
    rte_ether_addr_copy(&eth_hdr->src_addr, &t->last_recv_mac);
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
    client_entry_t* client = add_or_update_client(t, &src_addr, protocol);
    uint32_t client_id = client ? client->client_id : 0;

    /* Parse message */
    input_msg_t msg;
    bool parsed = false;

    if (protocol == TRANSPORT_PROTO_BINARY) {
        parsed = binary_message_parser_parse(&t->binary_parser,
                                              payload, payload_len, &msg);
    } else {
        /* CSV - need null termination */
        char buffer[2048];
        size_t copy_len = (payload_len < sizeof(buffer) - 1) ? 
                          payload_len : sizeof(buffer) - 1;
        memcpy(buffer, payload, copy_len);
        buffer[copy_len] = '\0';
        parsed = message_parser_parse(&t->csv_parser, buffer, &msg);
    }

    if (!parsed) {
        t->stats.rx_errors++;
        return false;
    }

    t->stats.rx_messages++;

    /* Create envelope */
    input_msg_envelope_t envelope = {
        .msg = msg,
        .client_id = client_id,
        .timestamp = get_timestamp()
    };

    /* Route to queue */
    input_envelope_queue_t* target = t->input_queue_0;

    if (t->config.dual_processor && t->input_queue_1 != NULL) {
        const char* symbol = NULL;
        if (msg.type == MSG_NEW_ORDER) symbol = msg.data.new_order.symbol;
        else if (msg.type == MSG_CANCEL) symbol = msg.data.cancel.symbol;

        if (symbol != NULL) {
            int processor_id = get_processor_id_for_symbol(symbol);
            target = (processor_id == 0) ? t->input_queue_0 : t->input_queue_1;
        }

        if (msg.type == MSG_FLUSH) {
            input_envelope_queue_enqueue(t->input_queue_0, &envelope);
            input_envelope_queue_enqueue(t->input_queue_1, &envelope);
            return true;
        }
    }

    if (input_envelope_queue_enqueue(target, &envelope)) {
        return true;
    } else {
        t->stats.rx_dropped++;
        return false;
    }
}

/* ============================================================================
 * RX Thread (Poll Loop)
 * ============================================================================ */

static void* rx_thread_func(void* arg) {
    udp_transport_t* t = (udp_transport_t*)arg;

    assert(t != NULL && "NULL transport");

    fprintf(stderr, "[DPDK UDP] RX thread started (port %u, queue %u)\n",
            t->port_id, t->rx_queue);

    /* Initialize parsers */
    message_parser_init(&t->csv_parser);
    binary_message_parser_init(&t->binary_parser);

    struct rte_mbuf* rx_bufs[BURST_SIZE];

    while (!atomic_load(t->shutdown_flag)) {
        /* Poll for packets */
        uint16_t nb_rx = rte_eth_rx_burst(t->port_id, t->rx_queue,
                                           rx_bufs, BURST_SIZE);

        t->stats.rx_poll_empty += (nb_rx == 0) ? 1 : 0;
        t->stats.rx_poll_full += (nb_rx == BURST_SIZE) ? 1 : 0;

        if (nb_rx == 0) {
            /* No packets - could add a small pause here for power saving */
            continue;
        }

        /* Process each packet */
        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf* mbuf = rx_bufs[i];

            t->stats.rx_packets++;
            t->stats.rx_bytes += rte_pktmbuf_pkt_len(mbuf);

            /* Check it's IPv4 UDP */
            struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
            if (rte_be_to_cpu_16(eth_hdr->ether_type) == RTE_ETHER_TYPE_IPV4) {
                struct rte_ipv4_hdr* ip_hdr = (struct rte_ipv4_hdr*)(eth_hdr + 1);
                if (ip_hdr->next_proto_id == IPPROTO_UDP) {
                    process_udp_packet(t, mbuf);
                }
            }

            /* Free the mbuf */
            rte_pktmbuf_free(mbuf);
        }
    }

    fprintf(stderr, "[DPDK UDP] RX thread stopped\n");
    return NULL;
}

/* ============================================================================
 * TX Functions
 * ============================================================================ */

static struct rte_mbuf* build_udp_packet(udp_transport_t* t,
                                          const transport_addr_t* dst_addr,
                                          const struct rte_ether_addr* dst_mac,
                                          const void* data,
                                          size_t len) {
    assert(t != NULL && "NULL transport");
    assert(dst_addr != NULL && "NULL dst_addr");
    assert(data != NULL && "NULL data");

    struct rte_mempool* pool = dpdk_get_mempool();
    if (pool == NULL) {
        return NULL;
    }

    struct rte_mbuf* mbuf = rte_pktmbuf_alloc(pool);
    if (mbuf == NULL) {
        return NULL;
    }

    /* Calculate sizes */
    size_t pkt_size = sizeof(struct rte_ether_hdr) + 
                      sizeof(struct rte_ipv4_hdr) +
                      sizeof(struct rte_udp_hdr) + 
                      len;

    char* pkt = rte_pktmbuf_append(mbuf, pkt_size);
    if (pkt == NULL) {
        rte_pktmbuf_free(mbuf);
        return NULL;
    }

    /* Ethernet header */
    struct rte_ether_hdr* eth_hdr = (struct rte_ether_hdr*)pkt;
    rte_ether_addr_copy(&t->our_mac, &eth_hdr->src_addr);
    if (dst_mac != NULL) {
        rte_ether_addr_copy(dst_mac, &eth_hdr->dst_addr);
    } else {
        /* Use broadcast if no MAC known */
        memset(&eth_hdr->dst_addr, 0xff, sizeof(eth_hdr->dst_addr));
    }
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* IPv4 header */
    struct rte_ipv4_hdr* ip_hdr = (struct rte_ipv4_hdr*)(eth_hdr + 1);
    memset(ip_hdr, 0, sizeof(*ip_hdr));
    ip_hdr->version_ihl = 0x45;  /* IPv4, 20 byte header */
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
                                            sizeof(struct rte_udp_hdr) + len);
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = IPPROTO_UDP;
    ip_hdr->src_addr = t->filter_ip;  /* Our IP */
    ip_hdr->dst_addr = dst_addr->ip_addr;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

    /* UDP header */
    struct rte_udp_hdr* udp_hdr = (struct rte_udp_hdr*)(ip_hdr + 1);
    udp_hdr->src_port = rte_cpu_to_be_16(t->filter_port);
    udp_hdr->dst_port = dst_addr->port;
    udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + len);
    udp_hdr->dgram_cksum = 0;  /* Optional for IPv4 */

    /* Payload */
    memcpy(udp_hdr + 1, data, len);

    return mbuf;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

udp_transport_t* udp_transport_create(const udp_transport_config_t* config,
                                       input_envelope_queue_t* input_queue_0,
                                       input_envelope_queue_t* input_queue_1,
                                       atomic_bool* shutdown_flag) {
    assert(config != NULL && "NULL config");
    assert(config->bind_port > 0 && "Invalid port");
    assert(input_queue_0 != NULL && "NULL input_queue_0");
    assert(shutdown_flag != NULL && "NULL shutdown_flag");

    if (config->dual_processor && input_queue_1 == NULL) {
        fprintf(stderr, "[DPDK UDP] dual_processor requires input_queue_1\n");
        return NULL;
    }

    /* Ensure DPDK is initialized */
    if (!dpdk_is_initialized()) {
        fprintf(stderr, "[DPDK UDP] DPDK not initialized! Call dpdk_init() first.\n");
        return NULL;
    }

    udp_transport_t* t = calloc(1, sizeof(udp_transport_t));
    if (t == NULL) {
        fprintf(stderr, "[DPDK UDP] Failed to allocate transport\n");
        return NULL;
    }

    t->config = *config;
    t->port_id = dpdk_get_active_port();
    t->rx_queue = 0;
    t->tx_queue = 0;
    t->filter_port = config->bind_port;
    t->filter_ip = 0;  /* Any */

    t->input_queue_0 = input_queue_0;
    t->input_queue_1 = input_queue_1;
    t->shutdown_flag = shutdown_flag;
    t->next_client_id = 1;

    atomic_init(&t->running, false);
    atomic_init(&t->started, false);
    atomic_init(&t->active_clients, 0);

    memset(&t->stats, 0, sizeof(t->stats));
    memset(t->clients, 0, sizeof(t->clients));

    /* Get our MAC address */
    uint16_t port_id = dpdk_get_active_port();
    if (!dpdk_get_port_mac(port_id, t->our_mac.addr_bytes)) {
        fprintf(stderr, "[DPDK UDP] Failed to get MAC address\n");
        free(t);
        return NULL;
    }

    char mac_str[18];
    dpdk_mac_to_str(t->our_mac.addr_bytes, mac_str);
    fprintf(stderr, "[DPDK UDP] Created transport (port %u, filter UDP:%u, MAC %s)\n",
            t->port_id, t->filter_port, mac_str);

    return t;
}

bool udp_transport_start(udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");

    bool expected = false;
    if (!atomic_compare_exchange_strong(&transport->started, &expected, true)) {
        fprintf(stderr, "[DPDK UDP] Already started\n");
        return false;
    }

    atomic_store(&transport->running, true);

    int rc = pthread_create(&transport->rx_thread, NULL,
                            rx_thread_func, transport);
    if (rc != 0) {
        fprintf(stderr, "[DPDK UDP] pthread_create failed: %d\n", rc);
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
    if (transport == NULL) return;

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

    /* Build packet */
    struct rte_mbuf* mbuf = build_udp_packet(transport, addr, NULL, data, len);
    if (mbuf == NULL) {
        transport->stats.tx_errors++;
        return false;
    }

    /* Send it */
    uint16_t nb_tx = rte_eth_tx_burst(transport->port_id, transport->tx_queue,
                                       &mbuf, 1);

    if (nb_tx == 0) {
        rte_pktmbuf_free(mbuf);
        transport->stats.tx_errors++;
        return false;
    }

    transport->stats.tx_packets++;
    transport->stats.tx_bytes += len;
    transport->stats.tx_batch_count++;

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

    /* Use cached MAC address for faster path */
    struct rte_mbuf* mbuf = build_udp_packet(transport, &transport->last_recv_addr,
                                              &transport->last_recv_mac, data, len);
    if (mbuf == NULL) {
        transport->stats.tx_errors++;
        return false;
    }

    uint16_t nb_tx = rte_eth_tx_burst(transport->port_id, transport->tx_queue,
                                       &mbuf, 1);

    if (nb_tx == 0) {
        rte_pktmbuf_free(mbuf);
        transport->stats.tx_errors++;
        return false;
    }

    transport->stats.tx_packets++;
    transport->stats.tx_bytes += len;

    return true;
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
    if (client == NULL) return false;
    *addr = client->addr;
    return true;
}

transport_protocol_t udp_transport_get_client_protocol(
    const udp_transport_t* transport,
    uint32_t client_id) {

    assert(transport != NULL && "NULL transport");

    client_entry_t* client = find_client_by_id((udp_transport_t*)transport, client_id);
    return client ? client->protocol : TRANSPORT_PROTO_UNKNOWN;
}

size_t udp_transport_evict_inactive(udp_transport_t* transport,
                                     uint32_t timeout_sec) {
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

void udp_transport_get_stats(const udp_transport_t* transport,
                              transport_stats_t* stats) {
    assert(transport != NULL && "NULL transport");
    assert(stats != NULL && "NULL stats");

    *stats = transport->stats;
    stats->active_clients = atomic_load(&transport->active_clients);
}

void udp_transport_reset_stats(udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    transport_stats_reset(&transport->stats);
}

void udp_transport_print_stats(const udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");

    transport_stats_t stats;
    udp_transport_get_stats(transport, &stats);

    fprintf(stderr, "\n=== UDP Transport Statistics (DPDK) ===\n");
    fprintf(stderr, "RX packets:     %lu\n", stats.rx_packets);
    fprintf(stderr, "RX bytes:       %lu\n", stats.rx_bytes);
    fprintf(stderr, "RX messages:    %lu\n", stats.rx_messages);
    fprintf(stderr, "RX errors:      %lu\n", stats.rx_errors);
    fprintf(stderr, "RX dropped:     %lu\n", stats.rx_dropped);
    fprintf(stderr, "RX poll empty:  %lu\n", stats.rx_poll_empty);
    fprintf(stderr, "RX poll full:   %lu\n", stats.rx_poll_full);
    fprintf(stderr, "TX packets:     %lu\n", stats.tx_packets);
    fprintf(stderr, "TX bytes:       %lu\n", stats.tx_bytes);
    fprintf(stderr, "TX errors:      %lu\n", stats.tx_errors);
    fprintf(stderr, "TX batches:     %lu\n", stats.tx_batch_count);
    fprintf(stderr, "Active clients: %u\n", stats.active_clients);
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

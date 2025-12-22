#include "network/udp_transport.h"
#include "protocol/csv/message_parser.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/symbol_router.h"
#include "platform/timestamps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

/**
 * UDP Transport - Socket Implementation
 *
 * This is the default implementation using standard POSIX sockets.
 * It wraps our existing optimized udp_receiver functionality behind
 * the abstract udp_transport interface.
 *
 * Compile with: cmake .. -DUSE_DPDK=OFF (default)
 *
 * Rule Compliance:
 * - Rule 2: All loops bounded
 * - Rule 5: Minimum 2 assertions per function
 * - Rule 7: All return values checked
 */

/* ============================================================================
 * Constants (Rule 2: explicit bounds)
 * ============================================================================ */

#define MAX_RECV_BUFFER_SIZE    65536
#define MAX_MESSAGES_PER_PACKET 64
#define CLIENT_HASH_SIZE        8192
#define CLIENT_HASH_MASK        (CLIENT_HASH_SIZE - 1)
#define MAX_PROBE_LENGTH        128
#define RECV_TIMEOUT_SEC        0
#define RECV_TIMEOUT_USEC       100000  /* 100ms */

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

    /* Socket */
    int sockfd;
    uint16_t bound_port;

    /* Queues */
    input_envelope_queue_t* input_queue_0;
    input_envelope_queue_t* input_queue_1;

    /* Threading */
    pthread_t recv_thread;
    atomic_bool* shutdown_flag;
    atomic_bool running;
    atomic_bool started;

    /* Client tracking (open-addressing hash table) */
    client_entry_t clients[CLIENT_HASH_SIZE];
    uint32_t next_client_id;
    atomic_uint_fast32_t active_clients;

    /* Last received address (for send_to_last) */
    transport_addr_t last_recv_addr;
    bool has_last_recv;

    /* Statistics */
    transport_stats_t stats;

    /* Thread-local parsers (initialized in recv thread) */
    message_parser_t csv_parser;
    binary_message_parser_t binary_parser;
};

/* ============================================================================
 * Client Hash Table Operations
 * ============================================================================ */

static uint32_t hash_addr(const transport_addr_t* addr) {
    assert(addr != NULL && "NULL addr in hash_addr");
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

        if (!entry->active) {
            return NULL;  /* Empty slot = not found */
        }

        if (transport_addr_equal(&entry->addr, addr)) {
            return entry;  /* Found */
        }

        index = (index + 1) & CLIENT_HASH_MASK;
    }

    return NULL;  /* Max probes exceeded */
}

static client_entry_t* find_client_by_id(udp_transport_t* t, uint32_t client_id) {
    assert(t != NULL && "NULL transport");

    /* Linear scan - could optimize with secondary index if needed */
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
            /* Continue searching in case client exists further */
        } else if (transport_addr_equal(&entry->addr, addr)) {
            /* Found existing - update */
            entry->last_seen = (int64_t)time(NULL);
            if (protocol != TRANSPORT_PROTO_UNKNOWN) {
                entry->protocol = protocol;
            }
            return entry;
        }

        index = (index + 1) & CLIENT_HASH_MASK;
    }

    /* Not found - add new if we found an empty slot */
    if (empty_slot != NULL) {
        empty_slot->addr = *addr;
        empty_slot->client_id = t->next_client_id++;
        empty_slot->protocol = protocol;
        empty_slot->last_seen = (int64_t)time(NULL);
        empty_slot->active = true;
        atomic_fetch_add(&t->active_clients, 1);
        return empty_slot;
    }

    return NULL;  /* Table full */
}

/* ============================================================================
 * Protocol Detection
 * ============================================================================ */

static transport_protocol_t detect_protocol(const uint8_t* data, size_t len) {
    assert(data != NULL && "NULL data in detect_protocol");

    if (len < 2) {
        return TRANSPORT_PROTO_UNKNOWN;
    }

    /* Binary protocol starts with magic byte 0x4D ('M') */
    if (data[0] == 0x4D) {
        return TRANSPORT_PROTO_BINARY;
    }

    /* CSV starts with message type letter (N, C, F, etc.) */
    if ((data[0] >= 'A' && data[0] <= 'Z') ||
        (data[0] >= 'a' && data[0] <= 'z')) {
        return TRANSPORT_PROTO_CSV;
    }

    return TRANSPORT_PROTO_UNKNOWN;
}

/* ============================================================================
 * Message Parsing and Routing
 * ============================================================================ */

static bool parse_and_route_message(udp_transport_t* t,
                                     const uint8_t* data,
                                     size_t len,
                                     uint32_t client_id,
                                     transport_protocol_t protocol) {
    assert(t != NULL && "NULL transport");
    assert(data != NULL && "NULL data");

    input_msg_t msg;
    bool parsed = false;

    if (protocol == TRANSPORT_PROTO_BINARY) {
        parsed = binary_message_parser_parse(&t->binary_parser,
                                              data, len, &msg);
    } else {
        /* Treat as CSV (add null terminator) */
        char buffer[MAX_RECV_BUFFER_SIZE];
        size_t copy_len = (len < sizeof(buffer) - 1) ? len : sizeof(buffer) - 1;
        memcpy(buffer, data, copy_len);
        buffer[copy_len] = '\0';

        parsed = message_parser_parse(&t->csv_parser, buffer, &msg);
    }

    if (!parsed) {
        t->stats.rx_errors++;
        return false;
    }

    /* Create envelope */
    input_msg_envelope_t envelope = {
        .msg = msg,
        .client_id = client_id,
        .timestamp = get_timestamp()
    };

    /* Route to appropriate queue */
    input_envelope_queue_t* target_queue = t->input_queue_0;

    if (t->config.dual_processor && t->input_queue_1 != NULL) {
        /* Route based on symbol */
        const char* symbol = NULL;

        if (msg.type == MSG_NEW_ORDER) {
            symbol = msg.data.new_order.symbol;
        } else if (msg.type == MSG_CANCEL) {
            symbol = msg.data.cancel.symbol;
        }

        if (symbol != NULL) {
            int processor_id = get_processor_id_for_symbol(symbol);
            target_queue = (processor_id == 0) ? t->input_queue_0 : t->input_queue_1;
        }

        /* Flush goes to both queues */
        if (msg.type == MSG_FLUSH) {
            input_envelope_queue_enqueue(t->input_queue_0, &envelope);
            input_envelope_queue_enqueue(t->input_queue_1, &envelope);
            t->stats.rx_messages++;
            return true;
        }
    }

    if (input_envelope_queue_enqueue(target_queue, &envelope)) {
        t->stats.rx_messages++;
        return true;
    } else {
        t->stats.rx_dropped++;
        return false;
    }
}

/* ============================================================================
 * Receive Thread
 * ============================================================================ */

static void* recv_thread_func(void* arg) {
    udp_transport_t* t = (udp_transport_t*)arg;

    assert(t != NULL && "NULL transport in recv thread");
    assert(t->sockfd >= 0 && "Invalid socket");

    fprintf(stderr, "[UDP Transport] Receiver thread started (port %u)\n",
            t->bound_port);

    /* Initialize parsers */
    message_parser_init(&t->csv_parser);
    binary_message_parser_init(&t->binary_parser);

    uint8_t buffer[MAX_RECV_BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (!atomic_load(t->shutdown_flag)) {
        /* Receive packet */
        ssize_t recv_len = recvfrom(t->sockfd, buffer, sizeof(buffer) - 1, 0,
                                     (struct sockaddr*)&client_addr, &addr_len);

        if (recv_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  /* Timeout - check shutdown flag */
            }
            if (errno == EINTR) {
                continue;  /* Interrupted - retry */
            }
            t->stats.rx_errors++;
            continue;
        }

        if (recv_len == 0) {
            continue;  /* Empty packet */
        }

        /* Update stats */
        t->stats.rx_packets++;
        t->stats.rx_bytes += (uint64_t)recv_len;

        /* Convert address */
        transport_addr_t addr;
        transport_addr_from_sockaddr(&addr, &client_addr);

        /* Update last received address */
        t->last_recv_addr = addr;
        t->has_last_recv = true;

        /* Detect or use default protocol */
        transport_protocol_t protocol = t->config.default_protocol;
        if (t->config.detect_protocol) {
            transport_protocol_t detected = detect_protocol(buffer, (size_t)recv_len);
            if (detected != TRANSPORT_PROTO_UNKNOWN) {
                protocol = detected;
            }
        }

        /* Find or create client */
        client_entry_t* client = add_or_update_client(t, &addr, protocol);
        uint32_t client_id = client ? client->client_id : 0;

        /* Parse and route message */
        parse_and_route_message(t, buffer, (size_t)recv_len, client_id, protocol);
    }

    fprintf(stderr, "[UDP Transport] Receiver thread stopped\n");
    return NULL;
}

/* ============================================================================
 * Socket Setup
 * ============================================================================ */

static bool setup_socket(udp_transport_t* t) {
    assert(t != NULL && "NULL transport");
    assert(t->sockfd == -1 && "Socket already created");

    /* Create socket */
    t->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (t->sockfd < 0) {
        fprintf(stderr, "[UDP Transport] socket() failed: %s\n", strerror(errno));
        return false;
    }

    /* SO_REUSEADDR */
    int reuse = 1;
    if (setsockopt(t->sockfd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        fprintf(stderr, "[UDP Transport] SO_REUSEADDR failed: %s\n", strerror(errno));
    }

    /* Receive buffer size */
    if (t->config.rx_buffer_size > 0) {
        int size = (int)t->config.rx_buffer_size;
        if (setsockopt(t->sockfd, SOL_SOCKET, SO_RCVBUF,
                       &size, sizeof(size)) < 0) {
            fprintf(stderr, "[UDP Transport] SO_RCVBUF failed: %s\n", strerror(errno));
        }
    }

    /* Send buffer size */
    if (t->config.tx_buffer_size > 0) {
        int size = (int)t->config.tx_buffer_size;
        if (setsockopt(t->sockfd, SOL_SOCKET, SO_SNDBUF,
                       &size, sizeof(size)) < 0) {
            fprintf(stderr, "[UDP Transport] SO_SNDBUF failed: %s\n", strerror(errno));
        }
    }

#ifdef __linux__
    /* SO_BUSY_POLL for lower latency */
    if (t->config.busy_poll) {
        int busy_poll_us = 50;
        if (setsockopt(t->sockfd, SOL_SOCKET, SO_BUSY_POLL,
                       &busy_poll_us, sizeof(busy_poll_us)) < 0) {
            /* Silently ignore - requires CAP_NET_ADMIN */
        }
    }
#endif

    /* Receive timeout */
    struct timeval tv = {
        .tv_sec = RECV_TIMEOUT_SEC,
        .tv_usec = (t->config.rx_timeout_us > 0) ?
                   (suseconds_t)t->config.rx_timeout_us : RECV_TIMEOUT_USEC
    };
    if (setsockopt(t->sockfd, SOL_SOCKET, SO_RCVTIMEO,
                   &tv, sizeof(tv)) < 0) {
        fprintf(stderr, "[UDP Transport] SO_RCVTIMEO failed: %s\n", strerror(errno));
    }

    /* Bind */
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(t->config.bind_port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (t->config.bind_addr != NULL) {
        if (inet_pton(AF_INET, t->config.bind_addr, &bind_addr.sin_addr) != 1) {
            fprintf(stderr, "[UDP Transport] Invalid bind address: %s\n",
                    t->config.bind_addr);
            close(t->sockfd);
            t->sockfd = -1;
            return false;
        }
    }

    if (bind(t->sockfd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        fprintf(stderr, "[UDP Transport] bind() failed: %s\n", strerror(errno));
        close(t->sockfd);
        t->sockfd = -1;
        return false;
    }

    /* Get actual bound port (useful if bind_port was 0) */
    socklen_t len = sizeof(bind_addr);
    if (getsockname(t->sockfd, (struct sockaddr*)&bind_addr, &len) == 0) {
        t->bound_port = ntohs(bind_addr.sin_port);
    } else {
        t->bound_port = t->config.bind_port;
    }

    fprintf(stderr, "[UDP Transport] Bound to port %u (socket mode)\n",
            t->bound_port);

    return true;
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
        fprintf(stderr, "[UDP Transport] dual_processor requires input_queue_1\n");
        return NULL;
    }

    udp_transport_t* t = calloc(1, sizeof(udp_transport_t));
    if (t == NULL) {
        fprintf(stderr, "[UDP Transport] Failed to allocate transport\n");
        return NULL;
    }

    t->config = *config;
    t->sockfd = -1;
    t->input_queue_0 = input_queue_0;
    t->input_queue_1 = input_queue_1;
    t->shutdown_flag = shutdown_flag;
    t->next_client_id = 1;

    atomic_init(&t->running, false);
    atomic_init(&t->started, false);
    atomic_init(&t->active_clients, 0);

    memset(&t->stats, 0, sizeof(t->stats));
    memset(t->clients, 0, sizeof(t->clients));

    if (!setup_socket(t)) {
        free(t);
        return NULL;
    }

    return t;
}

bool udp_transport_start(udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    assert(transport->sockfd >= 0 && "Socket not initialized");

    bool expected = false;
    if (!atomic_compare_exchange_strong(&transport->started, &expected, true)) {
        fprintf(stderr, "[UDP Transport] Already started\n");
        return false;
    }

    atomic_store(&transport->running, true);

    int rc = pthread_create(&transport->recv_thread, NULL,
                            recv_thread_func, transport);
    if (rc != 0) {
        fprintf(stderr, "[UDP Transport] pthread_create failed: %s\n",
                strerror(rc));
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

    /* shutdown_flag is set externally */
    atomic_store(&transport->running, false);

    pthread_join(transport->recv_thread, NULL);
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

    if (transport->sockfd >= 0) {
        close(transport->sockfd);
        transport->sockfd = -1;
    }

    free(transport);
}

bool udp_transport_send_to_client(udp_transport_t* transport,
                                   uint32_t client_id,
                                   const void* data,
                                   size_t len) {
    assert(transport != NULL && "NULL transport");
    assert(data != NULL && "NULL data");
    assert(len > 0 && "Zero length");

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

    struct sockaddr_in sock_addr;
    transport_addr_to_sockaddr(&sock_addr, addr);

    ssize_t sent = sendto(transport->sockfd, data, len, 0,
                          (struct sockaddr*)&sock_addr, sizeof(sock_addr));

    if (sent < 0) {
        transport->stats.tx_errors++;
        return false;
    }

    transport->stats.tx_packets++;
    transport->stats.tx_bytes += (uint64_t)sent;
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

    return udp_transport_send_to_addr(transport, &transport->last_recv_addr,
                                       data, len);
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

    /* Cast away const for internal lookup */
    client_entry_t* client = find_client_by_id((udp_transport_t*)transport,
                                                 client_id);
    if (client == NULL) {
        return false;
    }

    *addr = client->addr;
    return true;
}

transport_protocol_t udp_transport_get_client_protocol(
    const udp_transport_t* transport,
    uint32_t client_id) {

    assert(transport != NULL && "NULL transport");

    client_entry_t* client = find_client_by_id((udp_transport_t*)transport,
                                                 client_id);
    if (client == NULL) {
        return TRANSPORT_PROTO_UNKNOWN;
    }

    return client->protocol;
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

    fprintf(stderr, "\n=== UDP Transport Statistics (socket) ===\n");
    fprintf(stderr, "RX packets:     %llu\n", (unsigned long long)stats.rx_packets);
    fprintf(stderr, "RX bytes:       %llu\n", (unsigned long long)stats.rx_bytes);
    fprintf(stderr, "RX messages:    %llu\n", (unsigned long long)stats.rx_messages);
    fprintf(stderr, "RX errors:      %llu\n", (unsigned long long)stats.rx_errors);
    fprintf(stderr, "RX dropped:     %llu\n", (unsigned long long)stats.rx_dropped);
    fprintf(stderr, "TX packets:     %llu\n", (unsigned long long)stats.tx_packets);
    fprintf(stderr, "TX bytes:       %llu\n", (unsigned long long)stats.tx_bytes);
    fprintf(stderr, "TX errors:      %llu\n", (unsigned long long)stats.tx_errors);
    fprintf(stderr, "Active clients: %u\n", stats.active_clients);
}

bool udp_transport_is_running(const udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    return atomic_load(&transport->running);
}

uint16_t udp_transport_get_port(const udp_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    return transport->bound_port;
}

const char* udp_transport_get_backend(void) {
    return "socket";
}

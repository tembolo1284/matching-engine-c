/**
 * Multicast Transport - Socket Implementation
 *
 * Standard BSD socket implementation of the multicast transport interface.
 * This is the default backend when DPDK is not enabled.
 *
 * Compile with: cmake .. -DUSE_DPDK=OFF (default)
 *
 * Rule Compliance:
 * - Rule 2: All loops bounded (BATCH_SIZE, MAX_DRAIN_ITERATIONS)
 * - Rule 5: Minimum 2 assertions per function
 * - Rule 7: All return values checked
 */

#include "network/multicast_transport.h"
#include "network/transport_types.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_formatter.h"
#include "threading/queues.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <assert.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define BATCH_SIZE              32
#define MAX_DRAIN_ITERATIONS    100
#define MAX_OUTPUT_QUEUES       2

static const struct timespec idle_sleep = {
    .tv_sec = 0,
    .tv_nsec = 1000  /* 1 microsecond */
};

/* ============================================================================
 * Transport Structure
 * ============================================================================ */

struct multicast_transport {
    /* Configuration */
    multicast_transport_config_t config;
    
    /* Socket */
    int sockfd;
    struct sockaddr_in mcast_addr;
    
    /* Output queues to read from */
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
    
    struct in_addr in;
    if (inet_pton(AF_INET, addr, &in) != 1) {
        return false;
    }
    
    /* Multicast range: 224.0.0.0 - 239.255.255.255 */
    uint32_t ip = ntohl(in.s_addr);
    return (ip & 0xF0000000) == 0xE0000000;
}

/* ============================================================================
 * Socket Setup
 * ============================================================================ */

static bool setup_socket(multicast_transport_t* t) {
    assert(t != NULL && "NULL transport");
    
    /* Create UDP socket */
    t->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (t->sockfd < 0) {
        fprintf(stderr, "[Multicast] socket() failed: %s\n", strerror(errno));
        return false;
    }
    
    /* SO_REUSEADDR */
    int reuse = 1;
    if (setsockopt(t->sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        fprintf(stderr, "[Multicast] SO_REUSEADDR failed: %s\n", strerror(errno));
    }
    
    /* Send buffer size */
    if (t->config.tx_buffer_size > 0) {
        int tx_buf = (int)t->config.tx_buffer_size;
        if (setsockopt(t->sockfd, SOL_SOCKET, SO_SNDBUF, &tx_buf, sizeof(tx_buf)) < 0) {
            fprintf(stderr, "[Multicast] SO_SNDBUF failed: %s\n", strerror(errno));
        }
    }
    
    /* Multicast TTL */
    uint8_t ttl = t->config.ttl;
    if (setsockopt(t->sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        fprintf(stderr, "[Multicast] IP_MULTICAST_TTL failed: %s\n", strerror(errno));
    }
    
    /* Multicast loopback */
    uint8_t loop = t->config.loopback ? 1 : 0;
    if (setsockopt(t->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        fprintf(stderr, "[Multicast] IP_MULTICAST_LOOP failed: %s\n", strerror(errno));
    }
    
    /* Multicast interface */
    if (t->config.interface_addr != NULL && t->config.interface_addr[0] != '\0') {
        struct in_addr if_addr;
        if (inet_pton(AF_INET, t->config.interface_addr, &if_addr) == 1) {
            if (setsockopt(t->sockfd, IPPROTO_IP, IP_MULTICAST_IF, 
                           &if_addr, sizeof(if_addr)) < 0) {
                fprintf(stderr, "[Multicast] IP_MULTICAST_IF failed: %s\n", strerror(errno));
            }
        }
    }
    
    /* Setup destination address */
    memset(&t->mcast_addr, 0, sizeof(t->mcast_addr));
    t->mcast_addr.sin_family = AF_INET;
    t->mcast_addr.sin_port = htons(t->config.port);
    
    if (inet_pton(AF_INET, t->config.group_addr, &t->mcast_addr.sin_addr) != 1) {
        fprintf(stderr, "[Multicast] Invalid group address: %s\n", t->config.group_addr);
        close(t->sockfd);
        return false;
    }
    
    return true;
}

/* ============================================================================
 * Message Sending
 * ============================================================================ */

static bool send_message_internal(multicast_transport_t* t, const output_msg_t* msg) {
    assert(t != NULL && "NULL transport");
    assert(msg != NULL && "NULL msg");
    
    const char* text = NULL;
    const void* data = NULL;
    size_t len = 0;
    
    if (t->config.use_binary) {
        data = binary_message_formatter_format(&t->binary_formatter, msg, &len);
        if (data == NULL || len == 0) {
            t->stats.format_errors++;
            return false;
        }
    } else {
        text = message_formatter_format(&t->csv_formatter, msg);
        if (text == NULL) {
            t->stats.format_errors++;
            return false;
        }
        data = text;
        len = strlen(text);
    }
    
    ssize_t sent = sendto(t->sockfd, data, len, 0,
                          (struct sockaddr*)&t->mcast_addr, sizeof(t->mcast_addr));
    
    if (sent < 0) {
        t->stats.tx_errors++;
        return false;
    }
    
    t->stats.tx_packets++;
    t->stats.tx_bytes += (uint64_t)sent;
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
    
    fprintf(stderr, "[Multicast] Publisher thread started (%s:%u, %s)\n",
            t->config.group_addr, t->config.port,
            t->config.use_binary ? "binary" : "CSV");
    
    /* Initialize formatters */
    message_formatter_init(&t->csv_formatter);
    binary_message_formatter_init(&t->binary_formatter);
    
    output_msg_envelope_t batch[BATCH_SIZE];
    
    while (!atomic_load(t->shutdown_flag)) {
        size_t total_processed = 0;
        
        /* Round-robin across output queues */
        for (int q = 0; q < t->num_queues; q++) {
            output_envelope_queue_t* queue = t->output_queues[q];
            if (queue == NULL) continue;
            
            /* Dequeue batch */
            size_t count = 0;
            for (size_t i = 0; i < BATCH_SIZE; i++) {
                if (output_envelope_queue_dequeue(queue, &batch[count])) {
                    count++;
                } else {
                    break;
                }
            }
            
            /* Send each message */
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
    
    /* Drain remaining messages on shutdown */
    fprintf(stderr, "[Multicast] Draining remaining messages...\n");
    
    for (int iter = 0; iter < MAX_DRAIN_ITERATIONS; iter++) {
        bool has_messages = false;
        
        for (int q = 0; q < t->num_queues; q++) {
            output_envelope_queue_t* queue = t->output_queues[q];
            if (queue == NULL) continue;
            
            output_msg_envelope_t envelope;
            while (output_envelope_queue_dequeue(queue, &envelope)) {
                has_messages = true;
                if (send_message_internal(t, &envelope.msg)) {
                    if (q == 0) {
                        t->stats.messages_from_queue_0++;
                    } else {
                        t->stats.messages_from_queue_1++;
                    }
                }
            }
        }
        
        if (!has_messages) {
            break;
        }
    }
    
    fprintf(stderr, "[Multicast] Publisher thread stopped\n");
    return NULL;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

void multicast_transport_config_init(multicast_transport_config_t* config) {
    assert(config != NULL && "NULL config");
    
    memset(config, 0, sizeof(*config));
    config->group_addr = NULL;
    config->port = 0;
    config->use_binary = false;
    config->ttl = MULTICAST_TTL_LOCAL;
    config->loopback = false;
    config->interface_addr = NULL;
    config->tx_buffer_size = 4 * 1024 * 1024;  /* 4 MB */
}

multicast_transport_t* multicast_transport_create(
    const multicast_transport_config_t* config,
    output_envelope_queue_t* output_queue_0,
    output_envelope_queue_t* output_queue_1,
    atomic_bool* shutdown_flag) {
    
    assert(config != NULL && "NULL config");
    assert(config->group_addr != NULL && "NULL group_addr");
    assert(config->port > 0 && "Invalid port");
    assert(output_queue_0 != NULL && "NULL output_queue_0");
    assert(shutdown_flag != NULL && "NULL shutdown_flag");
    
    if (!multicast_address_is_valid(config->group_addr)) {
        fprintf(stderr, "[Multicast] Invalid multicast address: %s\n", config->group_addr);
        return NULL;
    }
    
    multicast_transport_t* t = calloc(1, sizeof(multicast_transport_t));
    if (t == NULL) {
        fprintf(stderr, "[Multicast] Failed to allocate transport\n");
        return NULL;
    }
    
    t->config = *config;
    t->sockfd = -1;
    t->output_queues[0] = output_queue_0;
    t->output_queues[1] = output_queue_1;
    t->num_queues = (output_queue_1 != NULL) ? 2 : 1;
    t->shutdown_flag = shutdown_flag;
    
    atomic_init(&t->running, false);
    atomic_init(&t->started, false);
    
    memset(&t->stats, 0, sizeof(t->stats));
    
    /* Setup socket */
    if (!setup_socket(t)) {
        free(t);
        return NULL;
    }
    
    fprintf(stderr, "[Multicast] Created transport (%s:%u, TTL=%u, %s)\n",
            config->group_addr, config->port, config->ttl,
            config->use_binary ? "binary" : "CSV");
    
    return t;
}

bool multicast_transport_start(multicast_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    
    bool expected = false;
    if (!atomic_compare_exchange_strong(&transport->started, &expected, true)) {
        fprintf(stderr, "[Multicast] Already started\n");
        return false;
    }
    
    atomic_store(&transport->running, true);
    
    int rc = pthread_create(&transport->publisher_thread, NULL, 
                            publisher_thread_func, transport);
    if (rc != 0) {
        fprintf(stderr, "[Multicast] pthread_create failed: %d\n", rc);
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
    if (transport == NULL) {
        return;
    }
    
    if (atomic_load(&transport->started)) {
        multicast_transport_stop(transport);
    }
    
    if (transport->sockfd >= 0) {
        close(transport->sockfd);
    }
    
    free(transport);
}

bool multicast_transport_send(multicast_transport_t* transport,
                               const void* data,
                               size_t len) {
    assert(transport != NULL && "NULL transport");
    assert(data != NULL && "NULL data");
    
    ssize_t sent = sendto(transport->sockfd, data, len, 0,
                          (struct sockaddr*)&transport->mcast_addr,
                          sizeof(transport->mcast_addr));
    
    if (sent < 0) {
        transport->stats.tx_errors++;
        return false;
    }
    
    transport->stats.tx_packets++;
    transport->stats.tx_bytes += (uint64_t)sent;
    
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
    /* Note: sequence is NOT reset */
}

void multicast_transport_print_stats(const multicast_transport_t* transport) {
    assert(transport != NULL && "NULL transport");
    
    fprintf(stderr, "\n=== Multicast Transport Statistics ===\n");
    fprintf(stderr, "Group:          %s:%u\n", 
            transport->config.group_addr, transport->config.port);
    fprintf(stderr, "Protocol:       %s\n", 
            transport->config.use_binary ? "binary" : "CSV");
    fprintf(stderr, "TX packets:     %lu\n", transport->stats.tx_packets);
    fprintf(stderr, "TX bytes:       %lu\n", transport->stats.tx_bytes);
    fprintf(stderr, "TX messages:    %lu\n", transport->stats.tx_messages);
    fprintf(stderr, "TX errors:      %lu\n", transport->stats.tx_errors);
    fprintf(stderr, "From queue 0:   %lu\n", transport->stats.messages_from_queue_0);
    fprintf(stderr, "From queue 1:   %lu\n", transport->stats.messages_from_queue_1);
    fprintf(stderr, "Format errors:  %lu\n", transport->stats.format_errors);
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
    return "socket";
}

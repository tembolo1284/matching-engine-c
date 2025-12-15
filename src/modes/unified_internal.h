#ifndef UNIFIED_INTERNAL_H
#define UNIFIED_INTERNAL_H

/**
 * Unified Server - Internal shared structures and declarations
 * 
 * This header is for internal use by the unified_*.c files only.
 * External code should use unified_mode.h
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#include "modes/unified_mode.h"
#include "core/matching_engine.h"
#include "threading/client_registry.h"
#include "threading/queues.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_formatter.h"
#include "network/multicast_publisher.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define TCP_LISTEN_BACKLOG 128
#define TCP_RECV_BUFFER_SIZE 65536
#define UDP_RECV_BUFFER_SIZE (10 * 1024 * 1024)  /* 10 MB */
#define MAX_UDP_PACKET_SIZE 65507

/* ============================================================================
 * Unified Server Context
 * ============================================================================ */

typedef struct {
    /* Configuration */
    unified_config_t config;
    
    /* Matching engine (shared by all processors) */
    memory_pools_t* pools_0;
    memory_pools_t* pools_1;  /* NULL if single processor */
    matching_engine_t* engine_0;
    matching_engine_t* engine_1;  /* NULL if single processor */
    
    /* Queues */
    input_envelope_queue_t* input_queue_0;
    input_envelope_queue_t* input_queue_1;  /* NULL if single processor */
    output_envelope_queue_t* output_queue_0;
    output_envelope_queue_t* output_queue_1;  /* NULL if single processor */
    
    /* Client tracking */
    client_registry_t* registry;
    user_client_map_t* user_map;
    
    /* Network */
    int tcp_listen_fd;
    int udp_fd;
    multicast_publisher_t* multicast;
    
    /* Formatters (for output) */
    binary_message_formatter_t bin_formatter;
    message_formatter_t csv_formatter;
    
    /* Statistics */
    atomic_uint_fast64_t tcp_messages_received;
    atomic_uint_fast64_t udp_messages_received;
    atomic_uint_fast64_t messages_routed;
    atomic_uint_fast64_t multicast_messages;
    atomic_uint_fast64_t tob_broadcasts;
} unified_server_t;

/* ============================================================================
 * External Shutdown Flag
 * ============================================================================ */

extern atomic_bool g_shutdown;

/* ============================================================================
 * Helper Functions (shared across modules)
 * ============================================================================ */

/**
 * Get current timestamp in nanoseconds
 */
static inline int64_t get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/**
 * Symbol routing: A-M = 0, N-Z = 1
 */
static inline int get_processor_for_symbol(const char* symbol) {
    if (!symbol || symbol[0] == '\0') return 0;
    char first = symbol[0];
    if (first >= 'a' && first <= 'z') {
        first = first - 'a' + 'A';  /* Uppercase */
    }
    if (first >= 'A' && first <= 'M') {
        return 0;
    }
    return 1;
}

/**
 * Detect protocol from message data
 */
client_protocol_t unified_detect_protocol(const uint8_t* data, size_t len);

/**
 * Route input message to appropriate processor queue
 */
void unified_route_input(unified_server_t* server, 
                         const input_msg_t* input,
                         uint32_t client_id,
                         const udp_client_addr_t* udp_addr);

/* ============================================================================
 * Thread Entry Points
 * ============================================================================ */

/**
 * TCP listener thread - accepts connections and spawns handlers
 */
void* unified_tcp_listener_thread(void* arg);

/**
 * UDP receiver thread - receives datagrams and routes to processors
 */
void* unified_udp_receiver_thread(void* arg);

/**
 * Output router thread - routes outputs to clients and multicast
 */
void* unified_output_router_thread(void* arg);

/* ============================================================================
 * Output Sending
 * ============================================================================ */

/**
 * Send output message to a specific client
 * Automatically formats based on client's protocol
 */
bool unified_send_to_client(unified_server_t* server, 
                            uint32_t client_id,
                            const output_msg_t* msg);

/**
 * Broadcast message to all connected clients
 */
void unified_broadcast_to_all(unified_server_t* server, const output_msg_t* msg);

#endif /* UNIFIED_INTERNAL_H */

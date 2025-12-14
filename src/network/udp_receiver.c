#include "network/udp_receiver.h"
#include "protocol/binary/binary_protocol.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/symbol_router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sched.h>

/* ============================================================================
 * Client Map Implementation (O(1) Hash Table)
 * ============================================================================ */

/**
 * Hash function for UDP address (FNV-1a inspired)
 */
static uint32_t hash_client_addr(const udp_client_addr_t* addr) {
    uint32_t h = 2166136261u;
    h ^= addr->addr;
    h *= 16777619u;
    h ^= addr->port;
    h *= 16777619u;
    return h;
}

/**
 * Initialize client map
 */
static void client_map_init(udp_client_map_t* map) {
    memset(map->entries, 0, sizeof(map->entries));
    for (size_t i = 0; i < UDP_CLIENT_HASH_SIZE; i++) {
        map->entries[i].active = false;
    }
    map->count = 0;
    map->next_id = CLIENT_ID_UDP_BASE + 1;
}

/**
 * Find slot for address (existing or empty)
 * Uses linear probing with bounded search
 */
static bool client_map_find_slot(udp_client_map_t* map,
                                  const udp_client_addr_t* addr,
                                  uint32_t* out_idx,
                                  bool* out_exists) {
    uint32_t hash = hash_client_addr(addr);
    uint32_t idx = hash & (UDP_CLIENT_HASH_SIZE - 1);
    uint32_t first_empty = UINT32_MAX;
    
    /* Linear probing bounded by table size */
    for (uint32_t probe = 0; probe < UDP_CLIENT_HASH_SIZE; probe++) {
        udp_client_entry_t* entry = &map->entries[idx];
        
        if (entry->active) {
            if (udp_client_addr_equal(&entry->addr, addr)) {
                *out_idx = idx;
                *out_exists = true;
                return true;
            }
        } else {
            if (first_empty == UINT32_MAX) {
                first_empty = idx;
            }
            /* Empty slot = not found (no tombstones in this implementation) */
            break;
        }
        
        idx = (idx + 1) & (UDP_CLIENT_HASH_SIZE - 1);
    }
    
    /* Not found, return first empty slot */
    if (first_empty != UINT32_MAX) {
        *out_idx = first_empty;
        *out_exists = false;
        return true;
    }
    
    return false;  /* Table full and no match */
}

/**
 * Evict oldest (LRU) entry
 */
static void client_map_evict_oldest(udp_client_map_t* map) {
    uint32_t oldest_idx = 0;
    int64_t oldest_time = INT64_MAX;
    bool found = false;
    
    for (size_t i = 0; i < UDP_CLIENT_HASH_SIZE; i++) {
        if (map->entries[i].active && map->entries[i].last_seen < oldest_time) {
            oldest_time = map->entries[i].last_seen;
            oldest_idx = (uint32_t)i;
            found = true;
        }
    }
    
    if (found) {
        map->entries[oldest_idx].active = false;
        if (map->count > 0) {
            map->count--;
        }
    }
}

/**
 * Get or create client entry
 */
static uint32_t client_map_get_or_create(udp_client_map_t* map,
                                          const udp_client_addr_t* addr) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec;
    
    uint32_t idx;
    bool exists;
    
    if (!client_map_find_slot(map, addr, &idx, &exists)) {
        /* Table completely full - shouldn't happen if we evict properly */
        client_map_evict_oldest(map);
        if (!client_map_find_slot(map, addr, &idx, &exists)) {
            return CLIENT_ID_BROADCAST;  /* Fallback */
        }
    }
    
    if (exists) {
        /* Update last seen */
        map->entries[idx].last_seen = now;
        return map->entries[idx].client_id;
    }
    
    /* Create new entry */
    if (map->count >= MAX_UDP_CLIENTS) {
        client_map_evict_oldest(map);
    }
    
    udp_client_entry_t* entry = &map->entries[idx];
    entry->addr = *addr;
    entry->client_id = map->next_id++;
    entry->last_seen = now;
    entry->protocol = CLIENT_PROTOCOL_UNKNOWN;
    entry->active = true;
    map->count++;
    
    /* Wrap next_id if needed (stay in UDP range) */
    if (map->next_id < CLIENT_ID_UDP_BASE) {
        map->next_id = CLIENT_ID_UDP_BASE + 1;
    }
    
    return entry->client_id;
}

/**
 * Find address by client ID (O(n) scan - called on send)
 */
static bool client_map_find_addr(const udp_client_map_t* map,
                                  uint32_t client_id,
                                  udp_client_addr_t* out_addr) {
    for (size_t i = 0; i < UDP_CLIENT_HASH_SIZE; i++) {
        if (map->entries[i].active && map->entries[i].client_id == client_id) {
            *out_addr = map->entries[i].addr;
            return true;
        }
    }
    return false;
}

/**
 * Get protocol for client
 */
static client_protocol_t client_map_get_protocol(const udp_client_map_t* map,
                                                  uint32_t client_id) {
    for (size_t i = 0; i < UDP_CLIENT_HASH_SIZE; i++) {
        if (map->entries[i].active && map->entries[i].client_id == client_id) {
            return map->entries[i].protocol;
        }
    }
    return CLIENT_PROTOCOL_UNKNOWN;
}

/**
 * Set protocol for address (only if unknown)
 */
static void client_map_set_protocol(udp_client_map_t* map,
                                     const udp_client_addr_t* addr,
                                     client_protocol_t protocol) {
    uint32_t idx;
    bool exists;
    
    if (client_map_find_slot(map, addr, &idx, &exists) && exists) {
        if (map->entries[idx].protocol == CLIENT_PROTOCOL_UNKNOWN) {
            map->entries[idx].protocol = protocol;
        }
    }
}

/* ============================================================================
 * Symbol Routing
 * ============================================================================ */

static const char* get_symbol_from_input_msg(const input_msg_t* msg) {
    switch (msg->type) {
        case INPUT_MSG_NEW_ORDER:
            return msg->data.new_order.symbol;
        case INPUT_MSG_CANCEL:
            return msg->data.cancel.symbol;
        case INPUT_MSG_FLUSH:
            return NULL;
        default:
            return NULL;
    }
}

static bool route_message(udp_receiver_t* receiver, 
                          const input_msg_envelope_t* envelope,
                          const input_msg_t* msg) {
    
    if (receiver->num_output_queues == 1) {
        int retry_count = 0;
        const int MAX_RETRIES = 1000;
        
        while (!input_envelope_queue_enqueue(receiver->output_queues[0], envelope)) {
            retry_count++;
            if (retry_count >= MAX_RETRIES) {
                return false;
            }
            sched_yield();
        }
        atomic_fetch_add(&receiver->messages_to_processor[0], 1);
        return true;
    }
    
    const char* symbol = get_symbol_from_input_msg(msg);
    
    if (msg->type == INPUT_MSG_FLUSH || !symbol_is_valid(symbol)) {
        bool success_0 = false;
        bool success_1 = false;
        int retry_count = 0;
        const int MAX_RETRIES = 1000;
        
        while (!success_0 && retry_count < MAX_RETRIES) {
            if (input_envelope_queue_enqueue(receiver->output_queues[0], envelope)) {
                success_0 = true;
                atomic_fetch_add(&receiver->messages_to_processor[0], 1);
            } else {
                retry_count++;
                sched_yield();
            }
        }
        
        retry_count = 0;
        while (!success_1 && retry_count < MAX_RETRIES) {
            if (input_envelope_queue_enqueue(receiver->output_queues[1], envelope)) {
                success_1 = true;
                atomic_fetch_add(&receiver->messages_to_processor[1], 1);
            } else {
                retry_count++;
                sched_yield();
            }
        }
        
        return success_0 && success_1;
    }
    
    int processor_id = get_processor_id_for_symbol(symbol);
    int retry_count = 0;
    const int MAX_RETRIES = 1000;
    
    while (!input_envelope_queue_enqueue(receiver->output_queues[processor_id], envelope)) {
        retry_count++;
        if (retry_count >= MAX_RETRIES) {
            return false;
        }
        sched_yield();
    }
    
    atomic_fetch_add(&receiver->messages_to_processor[processor_id], 1);
    return true;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

void udp_receiver_init(udp_receiver_t* receiver, 
                       input_envelope_queue_t* output_queue, 
                       uint16_t port) {
    memset(receiver, 0, sizeof(*receiver));
    
    receiver->output_queues[0] = output_queue;
    receiver->output_queues[1] = NULL;
    receiver->num_output_queues = 1;
    receiver->output_queue = output_queue;
    
    receiver->port = port;
    receiver->sockfd = -1;
    receiver->last_recv_len = 0;
    receiver->last_client_id = 0;
    
    client_map_init(&receiver->clients);
    
    message_parser_init(&receiver->csv_parser);
    binary_message_parser_init(&receiver->binary_parser);
    
    pthread_mutex_init(&receiver->send_lock, NULL);
    
    atomic_init(&receiver->running, false);
    atomic_init(&receiver->started, false);
    atomic_init(&receiver->packets_received, 0);
    atomic_init(&receiver->packets_sent, 0);
    atomic_init(&receiver->bytes_received, 0);
    atomic_init(&receiver->bytes_sent, 0);
    atomic_init(&receiver->messages_parsed, 0);
    atomic_init(&receiver->messages_dropped, 0);
    atomic_init(&receiver->send_errors, 0);
    atomic_init(&receiver->messages_to_processor[0], 0);
    atomic_init(&receiver->messages_to_processor[1], 0);
    atomic_init(&receiver->sequence, 0);
}

void udp_receiver_init_dual(udp_receiver_t* receiver,
                            input_envelope_queue_t* output_queue_0,
                            input_envelope_queue_t* output_queue_1,
                            uint16_t port) {
    memset(receiver, 0, sizeof(*receiver));
    
    receiver->output_queues[0] = output_queue_0;
    receiver->output_queues[1] = output_queue_1;
    receiver->num_output_queues = 2;
    receiver->output_queue = output_queue_0;
    
    receiver->port = port;
    receiver->sockfd = -1;
    receiver->last_recv_len = 0;
    receiver->last_client_id = 0;
    
    client_map_init(&receiver->clients);
    
    message_parser_init(&receiver->csv_parser);
    binary_message_parser_init(&receiver->binary_parser);
    
    pthread_mutex_init(&receiver->send_lock, NULL);
    
    atomic_init(&receiver->running, false);
    atomic_init(&receiver->started, false);
    atomic_init(&receiver->packets_received, 0);
    atomic_init(&receiver->packets_sent, 0);
    atomic_init(&receiver->bytes_received, 0);
    atomic_init(&receiver->bytes_sent, 0);
    atomic_init(&receiver->messages_parsed, 0);
    atomic_init(&receiver->messages_dropped, 0);
    atomic_init(&receiver->send_errors, 0);
    atomic_init(&receiver->messages_to_processor[0], 0);
    atomic_init(&receiver->messages_to_processor[1], 0);
    atomic_init(&receiver->sequence, 0);
}

void udp_receiver_destroy(udp_receiver_t* receiver) {
    udp_receiver_stop(receiver);
    
    if (receiver->sockfd >= 0) {
        close(receiver->sockfd);
        receiver->sockfd = -1;
    }
    
    pthread_mutex_destroy(&receiver->send_lock);
}

/* ============================================================================
 * Socket Setup
 * ============================================================================ */

bool udp_receiver_setup_socket(udp_receiver_t* receiver) {
    /* Create UDP socket with IPv4 */
    receiver->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver->sockfd < 0) {
        fprintf(stderr, "ERROR: Failed to create UDP socket: %s\n", strerror(errno));
        return false;
    }
    
    /* Set socket options */
    int reuse = 1;
    if (setsockopt(receiver->sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        fprintf(stderr, "WARNING: Failed to set SO_REUSEADDR: %s\n", strerror(errno));
    }
    
    /* Set large receive buffer */
    int buffer_size = UDP_RECV_BUFFER_SIZE;
    if (setsockopt(receiver->sockfd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
        fprintf(stderr, "WARNING: Failed to set receive buffer size: %s\n", strerror(errno));
    }
    
    /* Set large send buffer */
    buffer_size = UDP_SEND_BUFFER_SIZE;
    if (setsockopt(receiver->sockfd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
        fprintf(stderr, "WARNING: Failed to set send buffer size: %s\n", strerror(errno));
    }
    
    /* Verify buffer sizes */
    socklen_t optlen = sizeof(buffer_size);
    if (getsockopt(receiver->sockfd, SOL_SOCKET, SO_RCVBUF, &buffer_size, &optlen) == 0) {
        fprintf(stderr, "UDP socket receive buffer: %d bytes\n", buffer_size);
    }
    if (getsockopt(receiver->sockfd, SOL_SOCKET, SO_SNDBUF, &buffer_size, &optlen) == 0) {
        fprintf(stderr, "UDP socket send buffer: %d bytes\n", buffer_size);
    }
    
    /* Set receive timeout */
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;  /* 100ms */
    if (setsockopt(receiver->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        fprintf(stderr, "WARNING: Failed to set receive timeout: %s\n", strerror(errno));
    }
    
    /* Bind to port (IPv4) */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(receiver->port);
    
    if (bind(receiver->sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ERROR: Failed to bind to port %u: %s\n", receiver->port, strerror(errno));
        close(receiver->sockfd);
        receiver->sockfd = -1;
        return false;
    }
    
    fprintf(stderr, "UDP Server listening on port %u (bidirectional)\n", receiver->port);
    return true;
}

/* ============================================================================
 * Sending
 * ============================================================================ */

bool udp_receiver_send(udp_receiver_t* receiver,
                       uint32_t client_id,
                       const void* data,
                       size_t len) {
    if (!client_id_is_udp(client_id)) {
        return false;
    }
    
    udp_client_addr_t addr;
    if (!client_map_find_addr(&receiver->clients, client_id, &addr)) {
        atomic_fetch_add(&receiver->send_errors, 1);
        return false;
    }
    
    return udp_receiver_send_to_addr(receiver, &addr, data, len);
}

bool udp_receiver_send_to_last(udp_receiver_t* receiver,
                               const void* data,
                               size_t len) {
    if (receiver->last_recv_len == 0 || receiver->sockfd < 0) {
        return false;
    }
    
    pthread_mutex_lock(&receiver->send_lock);
    
    ssize_t sent = sendto(receiver->sockfd,
                          data,
                          len,
                          0,
                          (struct sockaddr*)&receiver->last_recv_addr,
                          receiver->last_recv_len);
    
    pthread_mutex_unlock(&receiver->send_lock);
    
    if (sent < 0) {
        atomic_fetch_add(&receiver->send_errors, 1);
        return false;
    }
    
    atomic_fetch_add(&receiver->packets_sent, 1);
    atomic_fetch_add(&receiver->bytes_sent, (uint64_t)sent);
    return true;
}

bool udp_receiver_send_to_addr(udp_receiver_t* receiver,
                               const udp_client_addr_t* addr,
                               const void* data,
                               size_t len) {
    if (receiver->sockfd < 0) {
        return false;
    }
    
    struct sockaddr_in sock_addr;
    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sin_family = AF_INET;
    sock_addr.sin_addr.s_addr = addr->addr;
    sock_addr.sin_port = addr->port;
    
    pthread_mutex_lock(&receiver->send_lock);
    
    ssize_t sent = sendto(receiver->sockfd,
                          data,
                          len,
                          0,
                          (struct sockaddr*)&sock_addr,
                          sizeof(sock_addr));
    
    pthread_mutex_unlock(&receiver->send_lock);
    
    if (sent < 0) {
        atomic_fetch_add(&receiver->send_errors, 1);
        return false;
    }
    
    atomic_fetch_add(&receiver->packets_sent, 1);
    atomic_fetch_add(&receiver->bytes_sent, (uint64_t)sent);
    return true;
}

client_protocol_t udp_receiver_get_client_protocol(const udp_receiver_t* receiver,
                                                    uint32_t client_id) {
    return client_map_get_protocol(&receiver->clients, client_id);
}

uint32_t udp_receiver_get_or_create_client(udp_receiver_t* receiver,
                                           const udp_client_addr_t* addr) {
    return client_map_get_or_create(&receiver->clients, addr);
}

bool udp_receiver_find_client_addr(const udp_receiver_t* receiver,
                                   uint32_t client_id,
                                   udp_client_addr_t* out_addr) {
    return client_map_find_addr(&receiver->clients, client_id, out_addr);
}

uint32_t udp_receiver_get_client_count(const udp_receiver_t* receiver) {
    return receiver->clients.count;
}

/* ============================================================================
 * Message Parsing
 * ============================================================================ */

static size_t get_binary_message_size(uint8_t msg_type) {
    switch (msg_type) {
        case BINARY_MSG_NEW_ORDER:
            return sizeof(binary_new_order_t);
        case BINARY_MSG_CANCEL:
            return sizeof(binary_cancel_t);
        case BINARY_MSG_FLUSH:
            return sizeof(binary_flush_t);
        default:
            return 0;
    }
}

static size_t parse_and_route_message(udp_receiver_t* receiver,
                                      const char* data,
                                      size_t remaining,
                                      uint32_t client_id,
                                      const udp_client_addr_t* client_addr) {
    input_msg_t msg;
    size_t consumed = 0;
    
    /* Check if this is a binary message */
    if (remaining >= 2 && (uint8_t)data[0] == BINARY_MAGIC) {
        uint8_t msg_type = (uint8_t)data[1];
        size_t msg_size = get_binary_message_size(msg_type);
        
        if (msg_size == 0) {
            return 1;  /* Skip unknown byte */
        }
        
        if (remaining < msg_size) {
            return 0;  /* Incomplete */
        }
        
        /* Set protocol for this client */
        client_map_set_protocol(&receiver->clients, client_addr, CLIENT_PROTOCOL_BINARY);
        
        if (binary_message_parser_parse(&receiver->binary_parser, data, msg_size, &msg)) {
            uint64_t seq = atomic_fetch_add(&receiver->sequence, 1);
            input_msg_envelope_t envelope = create_input_envelope_udp(&msg, client_id, 
                                                                       client_addr, seq);
            
            if (route_message(receiver, &envelope, &msg)) {
                atomic_fetch_add(&receiver->messages_parsed, 1);
            } else {
                atomic_fetch_add(&receiver->messages_dropped, 1);
            }
        }
        
        return msg_size;
    }
    
    /* CSV protocol */
    client_map_set_protocol(&receiver->clients, client_addr, CLIENT_PROTOCOL_CSV);
    
    const char* line_end = data;
    while (line_end < data + remaining && *line_end != '\n' && *line_end != '\r') {
        line_end++;
    }
    
    size_t line_len = line_end - data;
    
    if (line_len > 0) {
        char line_buffer[MAX_INPUT_LINE_LENGTH];
        if (line_len < MAX_INPUT_LINE_LENGTH) {
            memcpy(line_buffer, data, line_len);
            line_buffer[line_len] = '\0';
            
            if (message_parser_parse(&receiver->csv_parser, line_buffer, &msg)) {
                uint64_t seq = atomic_fetch_add(&receiver->sequence, 1);
                input_msg_envelope_t envelope = create_input_envelope_udp(&msg, client_id,
                                                                           client_addr, seq);
                
                if (route_message(receiver, &envelope, &msg)) {
                    atomic_fetch_add(&receiver->messages_parsed, 1);
                } else {
                    atomic_fetch_add(&receiver->messages_dropped, 1);
                }
            }
        }
        consumed = line_len;
    }
    
    /* Skip trailing newlines */
    const char* skip = data + consumed;
    while (skip < data + remaining && (*skip == '\n' || *skip == '\r')) {
        skip++;
        consumed++;
    }
    
    return consumed > 0 ? consumed : 1;
}

/* ============================================================================
 * Receive Thread
 * ============================================================================ */

void* udp_receiver_thread_func(void* arg) {
    udp_receiver_t* receiver = (udp_receiver_t*)arg;
    
    fprintf(stderr, "UDP Receiver thread started on port %d (mode: %s, bidirectional)\n", 
            receiver->port,
            receiver->num_output_queues == 2 ? "dual-processor" : "single-processor");
    
    char buffer[MAX_UDP_PACKET_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len;
    
    while (atomic_load_explicit(&receiver->running, memory_order_acquire)) {
        client_len = sizeof(client_addr);
        
        ssize_t bytes_received = recvfrom(receiver->sockfd,
                                          buffer,
                                          MAX_UDP_PACKET_SIZE - 1,
                                          0,
                                          (struct sockaddr*)&client_addr,
                                          &client_len);
        
        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            fprintf(stderr, "ERROR: recvfrom failed: %s\n", strerror(errno));
            continue;
        }
        
        if (bytes_received == 0) {
            continue;
        }
        
        buffer[bytes_received] = '\0';
        
        atomic_fetch_add(&receiver->packets_received, 1);
        atomic_fetch_add(&receiver->bytes_received, (uint64_t)bytes_received);
        
        /* Extract compact client address */
        udp_client_addr_t compact_addr = {
            .addr = client_addr.sin_addr.s_addr,
            .port = client_addr.sin_port,
            ._pad = 0
        };
        
        /* Get or create client ID */
        uint32_t client_id = client_map_get_or_create(&receiver->clients, &compact_addr);
        
        /* Store last received address for fast-path responses */
        memset(&receiver->last_recv_addr, 0, sizeof(receiver->last_recv_addr));
        receiver->last_recv_addr.sin6_family = AF_INET;
        /* Store IPv4 address in IPv4-mapped format for compatibility */
        memcpy(&receiver->last_recv_addr, &client_addr, sizeof(client_addr));
        receiver->last_recv_len = client_len;
        receiver->last_client_addr = compact_addr;
        receiver->last_client_id = client_id;
        
        /* Parse all messages in packet */
        const char* ptr = buffer;
        size_t remaining = (size_t)bytes_received;
        
        while (remaining > 0) {
            size_t consumed = parse_and_route_message(receiver, ptr, remaining,
                                                       client_id, &compact_addr);
            if (consumed == 0) {
                break;
            }
            ptr += consumed;
            remaining -= consumed;
        }
    }
    
    fprintf(stderr, "UDP Receiver thread stopped.\n");
    udp_receiver_print_stats(receiver);
    
    return NULL;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

bool udp_receiver_start(udp_receiver_t* receiver) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&receiver->started, &expected, true)) {
        return false;
    }
    
    if (!udp_receiver_setup_socket(receiver)) {
        atomic_store(&receiver->started, false);
        return false;
    }
    
    atomic_store_explicit(&receiver->running, true, memory_order_release);
    
    int result = pthread_create(&receiver->thread, NULL, udp_receiver_thread_func, receiver);
    if (result != 0) {
        fprintf(stderr, "ERROR: Failed to create UDP receiver thread: %s\n", strerror(result));
        atomic_store(&receiver->running, false);
        atomic_store(&receiver->started, false);
        close(receiver->sockfd);
        receiver->sockfd = -1;
        return false;
    }
    
    return true;
}

void udp_receiver_stop(udp_receiver_t* receiver) {
    if (!atomic_load(&receiver->started)) {
        return;
    }
    
    atomic_store_explicit(&receiver->running, false, memory_order_release);
    pthread_join(receiver->thread, NULL);
    atomic_store(&receiver->started, false);
}

bool udp_receiver_is_running(const udp_receiver_t* receiver) {
    return atomic_load_explicit(&receiver->running, memory_order_acquire);
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

uint64_t udp_receiver_get_packets_received(const udp_receiver_t* receiver) {
    return atomic_load(&receiver->packets_received);
}

uint64_t udp_receiver_get_packets_sent(const udp_receiver_t* receiver) {
    return atomic_load(&receiver->packets_sent);
}

uint64_t udp_receiver_get_messages_parsed(const udp_receiver_t* receiver) {
    return atomic_load(&receiver->messages_parsed);
}

uint64_t udp_receiver_get_messages_dropped(const udp_receiver_t* receiver) {
    return atomic_load(&receiver->messages_dropped);
}

void udp_receiver_print_stats(const udp_receiver_t* receiver) {
    fprintf(stderr, "\n=== UDP Server Statistics ===\n");
    fprintf(stderr, "Packets received:      %llu\n",
            (unsigned long long)atomic_load(&receiver->packets_received));
    fprintf(stderr, "Packets sent:          %llu\n",
            (unsigned long long)atomic_load(&receiver->packets_sent));
    fprintf(stderr, "Bytes received:        %llu\n",
            (unsigned long long)atomic_load(&receiver->bytes_received));
    fprintf(stderr, "Bytes sent:            %llu\n",
            (unsigned long long)atomic_load(&receiver->bytes_sent));
    fprintf(stderr, "Messages parsed:       %llu\n",
            (unsigned long long)atomic_load(&receiver->messages_parsed));
    fprintf(stderr, "Messages dropped:      %llu\n",
            (unsigned long long)atomic_load(&receiver->messages_dropped));
    fprintf(stderr, "Send errors:           %llu\n",
            (unsigned long long)atomic_load(&receiver->send_errors));
    fprintf(stderr, "Active clients:        %u\n", receiver->clients.count);
    
    if (receiver->num_output_queues == 2) {
        fprintf(stderr, "Messages to Proc 0 (A-M): %llu\n",
                (unsigned long long)atomic_load(&receiver->messages_to_processor[0]));
        fprintf(stderr, "Messages to Proc 1 (N-Z): %llu\n",
                (unsigned long long)atomic_load(&receiver->messages_to_processor[1]));
    }
}

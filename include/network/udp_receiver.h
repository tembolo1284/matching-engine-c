#ifndef MATCHING_ENGINE_UDP_RECEIVER_H
#define MATCHING_ENGINE_UDP_RECEIVER_H

#include "protocol/message_types_extended.h"
#include "protocol/csv/message_parser.h"
#include "protocol/binary/binary_message_parser.h"
#include "threading/queues.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * UdpReceiver - Bidirectional UDP Server
 *
 * Features:
 * - Receive UDP messages and parse them (CSV or binary, auto-detect)
 * - Track client addresses for response routing with O(1) hash lookup
 * - Send responses back to clients
 * - Protocol auto-detection per client (binary vs CSV)
 * - LRU eviction when client table is full
 *
 * Thread Safety:
 * - Receive loop runs in dedicated thread
 * - Send functions are thread-safe (mutex protected)
 *
 * Dual-Processor Support:
 * - Routes messages by symbol to appropriate processor queue
 * - A-M symbols → input_queue_0
 * - N-Z symbols → input_queue_1
 * - Flush commands → BOTH queues
 *
 * Kernel Bypass Integration Points:
 * - [KB-1] udp_receiver_setup_socket() → DPDK port init + queue setup
 * - [KB-2] recvfrom() → rte_eth_rx_burst() + packet parsing
 * - [KB-3] sendto() → rte_eth_tx_burst() + packet construction
 * - [KB-4] Client tracking hash → same (no change needed)
 *
 * DPDK would replace socket layer entirely while keeping:
 * - Message parsing (reused)
 * - Client tracking (reused)
 * - Queue routing (reused)
 * - Statistics (reused)
 */

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define MAX_UDP_PACKET_SIZE     65507
#define UDP_RECV_BUFFER_SIZE    (10 * 1024 * 1024)  /* 10MB socket buffer */
#define UDP_SEND_BUFFER_SIZE    (4 * 1024 * 1024)   /* 4MB send buffer */
#define MAX_INPUT_LINE_LENGTH   256
#define MAX_INPUT_QUEUES        2

/* Client tracking */
#define MAX_UDP_CLIENTS         4096
#define UDP_CLIENT_HASH_SIZE    8192    /* 2x clients for good load factor */

_Static_assert((UDP_CLIENT_HASH_SIZE & (UDP_CLIENT_HASH_SIZE - 1)) == 0,
               "Hash size must be power of 2 for fast modulo");

/* ============================================================================
 * Client Tracking (O(1) Hash Table)
 * ============================================================================ */

/**
 * UDP Client Entry - tracks connected clients
 *
 * Cache-optimized layout (32 bytes for better alignment):
 *   0-7:   last_seen (8 bytes)
 *   8-15:  addr (8 bytes)
 *   16-19: client_id (4 bytes)
 *   20:    protocol (1 byte)
 *   21:    active (1 byte)
 *   22-31: padding (10 bytes for 32-byte alignment)
 */
typedef struct {
    int64_t last_seen;              /* Timestamp for LRU eviction */
    udp_client_addr_t addr;         /* Client address */
    uint32_t client_id;             /* Assigned client ID */
    client_protocol_t protocol;     /* Detected protocol (binary/CSV) */
    bool active;                    /* Slot in use */
    uint8_t _pad[10];               /* Pad to 32 bytes */
} udp_client_entry_t;

_Static_assert(sizeof(udp_client_entry_t) == 32,
               "udp_client_entry_t should be 32 bytes for cache efficiency");

/**
 * UDP Client Map - open-addressing hash table
 *
 * O(1) average lookup/insert, O(n) worst case (full table)
 */
typedef struct {
    udp_client_entry_t entries[UDP_CLIENT_HASH_SIZE];
    uint32_t count;             /* Active client count */
    uint32_t next_id;           /* Next client ID to assign */
} udp_client_map_t;

/* ============================================================================
 * UDP Receiver Context
 * ============================================================================ */

/**
 * UDP receiver state
 *
 * Kernel Bypass Notes:
 * - sockfd → DPDK port_id + queue_id
 * - recv_buffer → rte_mbuf pool
 * - send_buffer → rte_mbuf for tx
 * - All other fields reusable
 */
typedef struct {
    /* Output queues - supports dual-processor mode */
    input_envelope_queue_t* output_queues[MAX_INPUT_QUEUES];
    int num_output_queues;              /* 1 = single, 2 = dual processor */

    /* Legacy single-queue pointer for backward compatibility */
    input_envelope_queue_t* output_queue; /* Points to output_queues[0] */

    /* Network configuration [KB-1] */
    uint16_t port;
    int sockfd;                         /* Socket fd (-1 if not open) */

    /* Buffers [KB-2, KB-3] */
    char recv_buffer[MAX_UDP_PACKET_SIZE];
    char send_buffer[MAX_UDP_PACKET_SIZE];

    /* Client tracking [KB-4] - reusable with DPDK */
    udp_client_map_t clients;

    /* Last received address (fast-path optimization) */
    struct sockaddr_in6 last_recv_addr;
    socklen_t last_recv_len;
    udp_client_addr_t last_client_addr;
    uint32_t last_client_id;

    /* Thread management */
    pthread_t thread;
    pthread_mutex_t send_lock;
    atomic_bool running;
    atomic_bool started;

    /* Statistics (reusable with DPDK) */
    atomic_uint_fast64_t packets_received;
    atomic_uint_fast64_t packets_sent;
    atomic_uint_fast64_t bytes_received;
    atomic_uint_fast64_t bytes_sent;
    atomic_uint_fast64_t messages_parsed;
    atomic_uint_fast64_t messages_dropped;
    atomic_uint_fast64_t send_errors;
    atomic_uint_fast64_t messages_to_processor[MAX_INPUT_QUEUES];

    /* Message sequence number */
    atomic_uint_fast64_t sequence;

    /* Parsers (reusable with DPDK) */
    message_parser_t csv_parser;
    binary_message_parser_t binary_parser;

} udp_receiver_t;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize UDP receiver (single processor mode)
 *
 * @param receiver     Receiver to initialize
 * @param output_queue Queue for parsed messages
 * @param port         UDP port to listen on
 *
 * Preconditions:
 * - receiver != NULL
 * - output_queue != NULL
 */
void udp_receiver_init(udp_receiver_t* receiver,
                       input_envelope_queue_t* output_queue,
                       uint16_t port);

/**
 * Initialize UDP receiver for dual-processor mode
 *
 * @param receiver       Receiver to initialize
 * @param output_queue_0 Queue for A-M symbols
 * @param output_queue_1 Queue for N-Z symbols
 * @param port           UDP port to listen on
 */
void udp_receiver_init_dual(udp_receiver_t* receiver,
                            input_envelope_queue_t* output_queue_0,
                            input_envelope_queue_t* output_queue_1,
                            uint16_t port);

/**
 * Destroy UDP receiver and cleanup resources
 */
void udp_receiver_destroy(udp_receiver_t* receiver);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/**
 * Start receiving (spawns thread)
 *
 * @return true on success
 */
bool udp_receiver_start(udp_receiver_t* receiver);

/**
 * Stop receiving (signals thread to exit and waits)
 */
void udp_receiver_stop(udp_receiver_t* receiver);

/**
 * Check if thread is running
 */
bool udp_receiver_is_running(const udp_receiver_t* receiver);

/* ============================================================================
 * Sending (Thread-Safe) [KB-3]
 * ============================================================================ */

/**
 * Send data to client by client ID
 *
 * @param receiver  UDP receiver instance
 * @param client_id Target client ID (must be UDP client)
 * @param data      Data to send
 * @param len       Length of data
 * @return true if sent successfully
 *
 * Thread Safety: Mutex protected
 */
bool udp_receiver_send(udp_receiver_t* receiver,
                       uint32_t client_id,
                       const void* data,
                       size_t len);

/**
 * Send data to last received address (fastest path)
 *
 * Use when responding immediately to a request.
 */
bool udp_receiver_send_to_last(udp_receiver_t* receiver,
                               const void* data,
                               size_t len);

/**
 * Send data to specific address
 */
bool udp_receiver_send_to_addr(udp_receiver_t* receiver,
                               const udp_client_addr_t* addr,
                               const void* data,
                               size_t len);

/**
 * Get protocol type for a client (for response formatting)
 */
client_protocol_t udp_receiver_get_client_protocol(const udp_receiver_t* receiver,
                                                    uint32_t client_id);

/* ============================================================================
 * Client Management [KB-4]
 * ============================================================================ */

/**
 * Get or create client ID for address
 *
 * @return Client ID (always > CLIENT_ID_UDP_BASE for UDP clients)
 */
uint32_t udp_receiver_get_or_create_client(udp_receiver_t* receiver,
                                           const udp_client_addr_t* addr);

/**
 * Find client address by ID
 *
 * @return true if found
 */
bool udp_receiver_find_client_addr(const udp_receiver_t* receiver,
                                   uint32_t client_id,
                                   udp_client_addr_t* out_addr);

/**
 * Get number of active clients
 */
uint32_t udp_receiver_get_client_count(const udp_receiver_t* receiver);

/* ============================================================================
 * Statistics
 * ============================================================================ */

uint64_t udp_receiver_get_packets_received(const udp_receiver_t* receiver);
uint64_t udp_receiver_get_packets_sent(const udp_receiver_t* receiver);
uint64_t udp_receiver_get_messages_parsed(const udp_receiver_t* receiver);
uint64_t udp_receiver_get_messages_dropped(const udp_receiver_t* receiver);
void udp_receiver_print_stats(const udp_receiver_t* receiver);
void udp_receiver_reset_stats(udp_receiver_t* receiver);

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

void* udp_receiver_thread_func(void* arg);
void udp_receiver_handle_packet(udp_receiver_t* receiver,
                                const char* data,
                                size_t length);
bool udp_receiver_setup_socket(udp_receiver_t* receiver);

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_UDP_RECEIVER_H */

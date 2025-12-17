#ifndef CLIENT_REGISTRY_H
#define CLIENT_REGISTRY_H

/**
 * Client Registry - Unified tracking of TCP and UDP clients
 *
 * Provides a single registry for all connected clients, regardless of transport.
 * Thread-safe for concurrent access from TCP listener, UDP receiver, and output router.
 *
 * Power of Ten Compliance:
 * - Rule 2: All iteration loops bounded by CLIENT_REGISTRY_HASH_SIZE
 * - Rule 3: No dynamic allocation (fixed-size hash table)
 * - Rule 5: Assertions in all functions (see .c file)
 * - Rule 7: All pthread return values checked
 *
 * Performance Notes:
 * - client_entry_t is cache-line aligned to prevent false sharing
 * - Hash table with linear probing for O(1) average lookup
 * - Read-write lock allows concurrent readers
 */

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdalign.h>

#include "protocol/message_types_extended.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define MAX_REGISTERED_CLIENTS 8192
#define CLIENT_REGISTRY_HASH_SIZE 16384  /* 2x max for good load factor */

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Transport type for a client
 */
typedef enum {
    TRANSPORT_UNKNOWN = 0,
    TRANSPORT_TCP = 1,
    TRANSPORT_UDP = 2
} transport_type_t;

/**
 * Client entry in the registry
 *
 * PERFORMANCE: Cache-line aligned (64 bytes) to prevent false sharing.
 * When multiple threads access different clients, they won't cause
 * cache line bouncing.
 *
 * Layout (64 bytes total):
 *   0-7:   last_seen (8 bytes)
 *   8-15:  messages_sent (8 bytes, atomic)
 *   16-23: messages_received (8 bytes, atomic)
 *   24-31: handle union (8 bytes)
 *   32-35: client_id (4 bytes)
 *   36-39: transport (4 bytes)
 *   40:    protocol (1 byte)
 *   41:    active (1 byte)
 *   42-63: padding (22 bytes)
 */
typedef struct {
    int64_t last_seen;                      /* 8 bytes - Timestamp of last activity */

    atomic_uint_fast64_t messages_sent;     /* 8 bytes */
    atomic_uint_fast64_t messages_received; /* 8 bytes */

    /* Transport-specific handle */
    union {
        int tcp_fd;                         /* File descriptor for TCP clients */
        udp_client_addr_t udp_addr;         /* Address for UDP clients (8 bytes) */
    } handle;                               /* 8 bytes */

    uint32_t client_id;                     /* 4 bytes - Unique client identifier */
    transport_type_t transport;             /* 4 bytes - TCP or UDP */
    client_protocol_t protocol;             /* 1 byte - CSV or Binary */
    bool active;                            /* 1 byte - Slot in use */

    /* Explicit padding to cache line boundary */
    uint8_t _pad[22];
} client_entry_t;

/* Ensure cache line alignment */
_Static_assert(sizeof(client_entry_t) == CACHE_LINE_SIZE,
               "client_entry_t must be cache-line sized (64 bytes)");

/**
 * Client Registry - thread-safe container for all clients
 *
 * Memory layout optimized for cache efficiency:
 * - Atomic counters grouped together
 * - Lock on separate cache line from hot data
 */
typedef struct {
    /* Hash table for O(1) lookup by client_id */
    /* Each entry is cache-line aligned */
    alignas(CACHE_LINE_SIZE) client_entry_t entries[CLIENT_REGISTRY_HASH_SIZE];

    /* ID generation - on own cache line */
    alignas(CACHE_LINE_SIZE) atomic_uint_fast32_t next_tcp_id;
    atomic_uint_fast32_t next_udp_id;

    /* Statistics - grouped for locality */
    atomic_uint_fast32_t tcp_client_count;
    atomic_uint_fast32_t udp_client_count;
    atomic_uint_fast64_t total_tcp_connections;
    atomic_uint_fast64_t total_udp_connections;

    /* Thread safety - on own cache line to avoid contention */
    alignas(CACHE_LINE_SIZE) pthread_rwlock_t lock;
} client_registry_t;

/**
 * Iterator callback for broadcast operations
 * Return true to continue iteration, false to stop
 */
typedef bool (*client_iterator_fn)(const client_entry_t* client, void* user_data);

/* ============================================================================
 * Initialization / Cleanup
 * ============================================================================ */

/**
 * Initialize the client registry
 *
 * @param registry  Registry to initialize (must not be NULL)
 */
void client_registry_init(client_registry_t* registry);

/**
 * Destroy the client registry (cleanup resources)
 *
 * @param registry  Registry to destroy (must not be NULL)
 */
void client_registry_destroy(client_registry_t* registry);

/* ============================================================================
 * Client Registration
 * ============================================================================ */

/**
 * Register a new TCP client
 *
 * @param registry   The client registry (must not be NULL)
 * @param tcp_fd     TCP socket file descriptor (must be >= 0)
 * @return           Assigned client_id, or 0 on failure
 */
uint32_t client_registry_add_tcp(client_registry_t* registry, int tcp_fd);

/**
 * Register a new UDP client
 *
 * @param registry   The client registry (must not be NULL)
 * @param addr       UDP client address
 * @return           Assigned client_id, or 0 on failure
 */
uint32_t client_registry_add_udp(client_registry_t* registry, udp_client_addr_t addr);

/**
 * Get or create a UDP client entry
 * If client exists, returns existing ID. Otherwise creates new entry.
 *
 * @param registry   The client registry (must not be NULL)
 * @param addr       UDP client address
 * @return           Client ID (existing or new), or 0 on failure
 */
uint32_t client_registry_get_or_add_udp(client_registry_t* registry, udp_client_addr_t addr);

/**
 * Remove a client from the registry
 *
 * @param registry   The client registry (must not be NULL)
 * @param client_id  Client to remove
 * @return           true if removed, false if not found
 */
bool client_registry_remove(client_registry_t* registry, uint32_t client_id);

/* ============================================================================
 * Client Lookup
 * ============================================================================ */

/**
 * Find a client by ID
 *
 * @param registry   The client registry (must not be NULL)
 * @param client_id  Client ID to find
 * @param out_entry  Output: client entry (copied for thread safety), can be NULL
 * @return           true if found, false otherwise
 */
bool client_registry_find(client_registry_t* registry, uint32_t client_id,
                          client_entry_t* out_entry);

/**
 * Find a UDP client by address
 *
 * @param registry   The client registry (must not be NULL)
 * @param addr       UDP address to find
 * @param out_id     Output: client ID if found, can be NULL
 * @return           true if found, false otherwise
 */
bool client_registry_find_udp_by_addr(client_registry_t* registry,
                                       udp_client_addr_t addr,
                                       uint32_t* out_id);

/**
 * Get client's protocol format
 *
 * @param registry   The client registry (must not be NULL)
 * @param client_id  Client ID
 * @return           Protocol format, or CLIENT_PROTOCOL_UNKNOWN if not found
 */
client_protocol_t client_registry_get_protocol(client_registry_t* registry,
                                                uint32_t client_id);

/**
 * Get client's transport type
 *
 * @param registry   The client registry (must not be NULL)
 * @param client_id  Client ID
 * @return           Transport type, or TRANSPORT_UNKNOWN if not found
 */
transport_type_t client_registry_get_transport(client_registry_t* registry,
                                                uint32_t client_id);

/* ============================================================================
 * Client State Updates
 * ============================================================================ */

/**
 * Set client's protocol format (called on first message when detected)
 *
 * @param registry   The client registry (must not be NULL)
 * @param client_id  Client ID
 * @param protocol   Detected protocol format
 * @return           true if updated, false if not found
 */
bool client_registry_set_protocol(client_registry_t* registry,
                                   uint32_t client_id,
                                   client_protocol_t protocol);

/**
 * Update client's last activity timestamp
 *
 * @param registry   The client registry (must not be NULL)
 * @param client_id  Client ID
 */
void client_registry_touch(client_registry_t* registry, uint32_t client_id);

/**
 * Increment messages received counter
 *
 * @param registry   The client registry (must not be NULL)
 * @param client_id  Client ID
 */
void client_registry_inc_received(client_registry_t* registry, uint32_t client_id);

/**
 * Increment messages sent counter
 *
 * @param registry   The client registry (must not be NULL)
 * @param client_id  Client ID
 */
void client_registry_inc_sent(client_registry_t* registry, uint32_t client_id);

/* ============================================================================
 * Iteration (for broadcast)
 * ============================================================================ */

/**
 * Iterate over all active clients
 *
 * @param registry   The client registry (must not be NULL)
 * @param callback   Callback function (must not be NULL)
 * @param user_data  User data passed to callback
 * @return           Number of clients iterated
 */
uint32_t client_registry_foreach(client_registry_t* registry,
                                  client_iterator_fn callback,
                                  void* user_data);

/**
 * Iterate over TCP clients only
 *
 * @param registry   The client registry (must not be NULL)
 * @param callback   Callback function (must not be NULL)
 * @param user_data  User data passed to callback
 * @return           Number of clients iterated
 */
uint32_t client_registry_foreach_tcp(client_registry_t* registry,
                                      client_iterator_fn callback,
                                      void* user_data);

/**
 * Iterate over UDP clients only
 *
 * @param registry   The client registry (must not be NULL)
 * @param callback   Callback function (must not be NULL)
 * @param user_data  User data passed to callback
 * @return           Number of clients iterated
 */
uint32_t client_registry_foreach_udp(client_registry_t* registry,
                                      client_iterator_fn callback,
                                      void* user_data);

/**
 * Get array of all active client IDs
 *
 * @param registry   The client registry (must not be NULL)
 * @param out_ids    Output array (must not be NULL)
 * @param max_ids    Maximum IDs to return (must be > 0)
 * @return           Number of IDs written
 */
uint32_t client_registry_get_all_ids(client_registry_t* registry,
                                      uint32_t* out_ids,
                                      uint32_t max_ids);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Client registry statistics
 */
typedef struct {
    uint32_t tcp_clients_active;
    uint32_t udp_clients_active;
    uint64_t tcp_connections_total;
    uint64_t udp_connections_total;
} client_registry_stats_t;

/**
 * Get registry statistics
 *
 * @param registry   The client registry (must not be NULL)
 * @param out_stats  Output statistics (must not be NULL)
 */
void client_registry_get_stats(client_registry_t* registry,
                                client_registry_stats_t* out_stats);

/**
 * Print registry statistics to stderr
 *
 * @param registry   The client registry (must not be NULL)
 */
void client_registry_print_stats(client_registry_t* registry);

#endif /* CLIENT_REGISTRY_H */

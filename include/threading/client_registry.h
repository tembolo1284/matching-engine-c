#ifndef CLIENT_REGISTRY_H
#define CLIENT_REGISTRY_H

/**
 * Client Registry - Unified tracking of TCP and UDP clients
 * 
 * Provides a single registry for all connected clients, regardless of transport.
 * Thread-safe for concurrent access from TCP listener, UDP receiver, and output router.
 */

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

#include "protocol/message_types_extended.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define MAX_REGISTERED_CLIENTS 8192
#define CLIENT_REGISTRY_HASH_SIZE 16384  /* 2x max for good load factor */

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
 * Layout optimized for alignment (largest fields first)
 */
typedef struct {
    int64_t last_seen;              /* 8 bytes - Timestamp of last activity */
    
    atomic_uint_fast64_t messages_sent;     /* 8 bytes */
    atomic_uint_fast64_t messages_received; /* 8 bytes */
    
    /* Transport-specific handle */
    union {
        int tcp_fd;                  /* File descriptor for TCP clients */
        udp_client_addr_t udp_addr;  /* Address for UDP clients (8 bytes) */
    } handle;                        /* 8 bytes */
    
    uint32_t client_id;              /* 4 bytes - Unique client identifier */
    transport_type_t transport;      /* 4 bytes - TCP or UDP */
    client_protocol_t protocol;      /* 1 byte - CSV or Binary */
    bool active;                     /* 1 byte - Slot in use */
    uint8_t _pad[2];                 /* 2 bytes - Explicit padding */
} client_entry_t;

/**
 * Client Registry - thread-safe container for all clients
 */
typedef struct {
    /* Hash table for O(1) lookup by client_id */
    client_entry_t entries[CLIENT_REGISTRY_HASH_SIZE];
    
    /* ID generation */
    atomic_uint_fast32_t next_tcp_id;  /* Next TCP client ID (1 to 0x7FFFFFFF) */
    atomic_uint_fast32_t next_udp_id;  /* Next UDP client ID (0x80000001 to 0xFFFFFFFF) */
    
    /* Statistics */
    atomic_uint_fast32_t tcp_client_count;
    atomic_uint_fast32_t udp_client_count;
    atomic_uint_fast64_t total_tcp_connections;
    atomic_uint_fast64_t total_udp_connections;
    
    /* Thread safety */
    pthread_rwlock_t lock;  /* Read-write lock for concurrent access */
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
 */
void client_registry_init(client_registry_t* registry);

/**
 * Destroy the client registry (cleanup resources)
 */
void client_registry_destroy(client_registry_t* registry);

/* ============================================================================
 * Client Registration
 * ============================================================================ */

/**
 * Register a new TCP client
 * 
 * @param registry   The client registry
 * @param tcp_fd     TCP socket file descriptor
 * @return           Assigned client_id, or 0 on failure
 */
uint32_t client_registry_add_tcp(client_registry_t* registry, int tcp_fd);

/**
 * Register a new UDP client
 * 
 * @param registry   The client registry
 * @param addr       UDP client address
 * @return           Assigned client_id, or 0 on failure
 */
uint32_t client_registry_add_udp(client_registry_t* registry, udp_client_addr_t addr);

/**
 * Get or create a UDP client entry
 * If client exists, returns existing ID. Otherwise creates new entry.
 * 
 * @param registry   The client registry
 * @param addr       UDP client address
 * @return           Client ID (existing or new), or 0 on failure
 */
uint32_t client_registry_get_or_add_udp(client_registry_t* registry, udp_client_addr_t addr);

/**
 * Remove a client from the registry
 * 
 * @param registry   The client registry
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
 * @param registry   The client registry
 * @param client_id  Client ID to find
 * @param out_entry  Output: client entry (copied for thread safety)
 * @return           true if found, false otherwise
 */
bool client_registry_find(client_registry_t* registry, uint32_t client_id, 
                          client_entry_t* out_entry);

/**
 * Find a UDP client by address
 * 
 * @param registry   The client registry
 * @param addr       UDP address to find
 * @param out_id     Output: client ID if found
 * @return           true if found, false otherwise
 */
bool client_registry_find_udp_by_addr(client_registry_t* registry,
                                       udp_client_addr_t addr,
                                       uint32_t* out_id);

/**
 * Get client's protocol format
 * 
 * @param registry   The client registry
 * @param client_id  Client ID
 * @return           Protocol format, or CLIENT_PROTOCOL_UNKNOWN if not found
 */
client_protocol_t client_registry_get_protocol(client_registry_t* registry,
                                                uint32_t client_id);

/**
 * Get client's transport type
 * 
 * @param registry   The client registry
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
 * @param registry   The client registry
 * @param client_id  Client ID
 * @param protocol   Detected protocol format
 * @return           true if updated, false if not found
 */
bool client_registry_set_protocol(client_registry_t* registry,
                                   uint32_t client_id,
                                   client_protocol_t protocol);

/**
 * Update client's last activity timestamp
 */
void client_registry_touch(client_registry_t* registry, uint32_t client_id);

/**
 * Increment messages received counter
 */
void client_registry_inc_received(client_registry_t* registry, uint32_t client_id);

/**
 * Increment messages sent counter
 */
void client_registry_inc_sent(client_registry_t* registry, uint32_t client_id);

/* ============================================================================
 * Iteration (for broadcast)
 * ============================================================================ */

/**
 * Iterate over all active clients
 */
uint32_t client_registry_foreach(client_registry_t* registry,
                                  client_iterator_fn callback,
                                  void* user_data);

/**
 * Iterate over TCP clients only
 */
uint32_t client_registry_foreach_tcp(client_registry_t* registry,
                                      client_iterator_fn callback,
                                      void* user_data);

/**
 * Iterate over UDP clients only
 */
uint32_t client_registry_foreach_udp(client_registry_t* registry,
                                      client_iterator_fn callback,
                                      void* user_data);

/**
 * Get array of all active client IDs
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
 */
void client_registry_get_stats(client_registry_t* registry,
                                client_registry_stats_t* out_stats);

/**
 * Print registry statistics to stderr
 */
void client_registry_print_stats(client_registry_t* registry);

#endif /* CLIENT_REGISTRY_H */

#include "threading/client_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Internal Helpers
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
 * Hash function for client_id (FNV-1a)
 */
static inline uint32_t hash_client_id(uint32_t client_id) {
    uint32_t hash = 2166136261u;
    hash ^= (client_id & 0xFF);
    hash *= 16777619u;
    hash ^= ((client_id >> 8) & 0xFF);
    hash *= 16777619u;
    hash ^= ((client_id >> 16) & 0xFF);
    hash *= 16777619u;
    hash ^= ((client_id >> 24) & 0xFF);
    hash *= 16777619u;
    return hash;
}

/**
 * Find slot for client_id (linear probing)
 */
static int find_slot_by_id(client_registry_t* registry, uint32_t client_id, bool find_empty) {
    uint32_t hash = hash_client_id(client_id);
    uint32_t start = hash % CLIENT_REGISTRY_HASH_SIZE;
    
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE; i++) {
        uint32_t idx = (start + i) % CLIENT_REGISTRY_HASH_SIZE;
        client_entry_t* entry = &registry->entries[idx];
        
        if (!entry->active) {
            if (find_empty) {
                return (int)idx;
            }
            continue;
        }
        
        if (entry->client_id == client_id) {
            return (int)idx;
        }
    }
    
    return -1;
}

/**
 * Find slot for UDP address
 */
static int find_slot_by_udp_addr(client_registry_t* registry, udp_client_addr_t addr) {
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE; i++) {
        client_entry_t* entry = &registry->entries[i];
        
        if (entry->active && 
            entry->transport == TRANSPORT_UDP &&
            udp_client_addr_equal(&entry->handle.udp_addr, &addr)) {
            return (int)i;
        }
    }
    
    return -1;
}

/**
 * Find an empty slot
 */
static int find_empty_slot(client_registry_t* registry, uint32_t hint_id) {
    uint32_t hash = hash_client_id(hint_id);
    uint32_t start = hash % CLIENT_REGISTRY_HASH_SIZE;
    
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE; i++) {
        uint32_t idx = (start + i) % CLIENT_REGISTRY_HASH_SIZE;
        if (!registry->entries[idx].active) {
            return (int)idx;
        }
    }
    
    return -1;
}

/* ============================================================================
 * Initialization / Cleanup
 * ============================================================================ */

void client_registry_init(client_registry_t* registry) {
    memset(registry->entries, 0, sizeof(registry->entries));
    
    /* TCP IDs: 1 to 0x7FFFFFFF */
    atomic_store(&registry->next_tcp_id, 1);
    
    /* UDP IDs: 0x80000001 to 0xFFFFFFFF */
    atomic_store(&registry->next_udp_id, CLIENT_ID_UDP_BASE + 1);
    
    atomic_store(&registry->tcp_client_count, 0);
    atomic_store(&registry->udp_client_count, 0);
    atomic_store(&registry->total_tcp_connections, 0);
    atomic_store(&registry->total_udp_connections, 0);
    
    pthread_rwlock_init(&registry->lock, NULL);
}

void client_registry_destroy(client_registry_t* registry) {
    pthread_rwlock_destroy(&registry->lock);
}

/* ============================================================================
 * Client Registration
 * ============================================================================ */

uint32_t client_registry_add_tcp(client_registry_t* registry, int tcp_fd) {
    /* Generate new TCP client ID */
    uint32_t client_id = atomic_fetch_add(&registry->next_tcp_id, 1);
    
    /* Wrap around if needed (skip 0 and UDP range) */
    if (client_id >= CLIENT_ID_UDP_BASE) {
        client_id = 1;
        atomic_store(&registry->next_tcp_id, 2);
    }
    
    pthread_rwlock_wrlock(&registry->lock);
    
    int slot = find_empty_slot(registry, client_id);
    if (slot < 0) {
        pthread_rwlock_unlock(&registry->lock);
        fprintf(stderr, "[ClientRegistry] ERROR: Registry full, cannot add TCP client\n");
        return 0;
    }
    
    client_entry_t* entry = &registry->entries[slot];
    entry->client_id = client_id;
    entry->transport = TRANSPORT_TCP;
    entry->protocol = CLIENT_PROTOCOL_UNKNOWN;
    entry->handle.tcp_fd = tcp_fd;
    entry->last_seen = get_timestamp_ns();
    atomic_store(&entry->messages_sent, 0);
    atomic_store(&entry->messages_received, 0);
    entry->active = true;
    
    atomic_fetch_add(&registry->tcp_client_count, 1);
    atomic_fetch_add(&registry->total_tcp_connections, 1);
    
    pthread_rwlock_unlock(&registry->lock);
    
    return client_id;
}

uint32_t client_registry_add_udp(client_registry_t* registry, udp_client_addr_t addr) {
    /* Generate new UDP client ID */
    uint32_t client_id = atomic_fetch_add(&registry->next_udp_id, 1);
    
    /* Wrap around if needed */
    if (client_id == 0) {
        client_id = CLIENT_ID_UDP_BASE + 1;
        atomic_store(&registry->next_udp_id, CLIENT_ID_UDP_BASE + 2);
    }
    
    pthread_rwlock_wrlock(&registry->lock);
    
    int slot = find_empty_slot(registry, client_id);
    if (slot < 0) {
        pthread_rwlock_unlock(&registry->lock);
        fprintf(stderr, "[ClientRegistry] ERROR: Registry full, cannot add UDP client\n");
        return 0;
    }
    
    client_entry_t* entry = &registry->entries[slot];
    entry->client_id = client_id;
    entry->transport = TRANSPORT_UDP;
    entry->protocol = CLIENT_PROTOCOL_UNKNOWN;
    entry->handle.udp_addr = addr;
    entry->last_seen = get_timestamp_ns();
    atomic_store(&entry->messages_sent, 0);
    atomic_store(&entry->messages_received, 0);
    entry->active = true;
    
    atomic_fetch_add(&registry->udp_client_count, 1);
    atomic_fetch_add(&registry->total_udp_connections, 1);
    
    pthread_rwlock_unlock(&registry->lock);
    
    return client_id;
}

uint32_t client_registry_get_or_add_udp(client_registry_t* registry, udp_client_addr_t addr) {
    /* First try read lock to find existing */
    pthread_rwlock_rdlock(&registry->lock);
    
    int slot = find_slot_by_udp_addr(registry, addr);
    if (slot >= 0) {
        uint32_t client_id = registry->entries[slot].client_id;
        pthread_rwlock_unlock(&registry->lock);
        return client_id;
    }
    
    pthread_rwlock_unlock(&registry->lock);
    
    /* Not found - need to add */
    pthread_rwlock_wrlock(&registry->lock);
    
    /* Re-check under write lock */
    slot = find_slot_by_udp_addr(registry, addr);
    if (slot >= 0) {
        uint32_t client_id = registry->entries[slot].client_id;
        pthread_rwlock_unlock(&registry->lock);
        return client_id;
    }
    
    /* Generate new UDP client ID */
    uint32_t client_id = atomic_fetch_add(&registry->next_udp_id, 1);
    if (client_id == 0) {
        client_id = CLIENT_ID_UDP_BASE + 1;
        atomic_store(&registry->next_udp_id, CLIENT_ID_UDP_BASE + 2);
    }
    
    slot = find_empty_slot(registry, client_id);
    if (slot < 0) {
        pthread_rwlock_unlock(&registry->lock);
        fprintf(stderr, "[ClientRegistry] ERROR: Registry full, cannot add UDP client\n");
        return 0;
    }
    
    client_entry_t* entry = &registry->entries[slot];
    entry->client_id = client_id;
    entry->transport = TRANSPORT_UDP;
    entry->protocol = CLIENT_PROTOCOL_UNKNOWN;
    entry->handle.udp_addr = addr;
    entry->last_seen = get_timestamp_ns();
    atomic_store(&entry->messages_sent, 0);
    atomic_store(&entry->messages_received, 0);
    entry->active = true;
    
    atomic_fetch_add(&registry->udp_client_count, 1);
    atomic_fetch_add(&registry->total_udp_connections, 1);
    
    pthread_rwlock_unlock(&registry->lock);
    
    return client_id;
}

bool client_registry_remove(client_registry_t* registry, uint32_t client_id) {
    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return false;
    }
    
    pthread_rwlock_wrlock(&registry->lock);
    
    int slot = find_slot_by_id(registry, client_id, false);
    if (slot < 0) {
        pthread_rwlock_unlock(&registry->lock);
        return false;
    }
    
    client_entry_t* entry = &registry->entries[slot];
    
    if (entry->transport == TRANSPORT_TCP) {
        atomic_fetch_sub(&registry->tcp_client_count, 1);
    } else if (entry->transport == TRANSPORT_UDP) {
        atomic_fetch_sub(&registry->udp_client_count, 1);
    }
    
    entry->active = false;
    entry->client_id = 0;
    
    pthread_rwlock_unlock(&registry->lock);
    
    return true;
}

/* ============================================================================
 * Client Lookup
 * ============================================================================ */

bool client_registry_find(client_registry_t* registry, uint32_t client_id,
                          client_entry_t* out_entry) {
    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return false;
    }
    
    pthread_rwlock_rdlock(&registry->lock);
    
    int slot = find_slot_by_id(registry, client_id, false);
    if (slot < 0 || !registry->entries[slot].active) {
        pthread_rwlock_unlock(&registry->lock);
        return false;
    }
    
    if (out_entry) {
        *out_entry = registry->entries[slot];
    }
    
    pthread_rwlock_unlock(&registry->lock);
    
    return true;
}

bool client_registry_find_udp_by_addr(client_registry_t* registry,
                                       udp_client_addr_t addr,
                                       uint32_t* out_id) {
    pthread_rwlock_rdlock(&registry->lock);
    
    int slot = find_slot_by_udp_addr(registry, addr);
    if (slot < 0) {
        pthread_rwlock_unlock(&registry->lock);
        return false;
    }
    
    if (out_id) {
        *out_id = registry->entries[slot].client_id;
    }
    
    pthread_rwlock_unlock(&registry->lock);
    
    return true;
}

client_protocol_t client_registry_get_protocol(client_registry_t* registry,
                                                uint32_t client_id) {
    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return CLIENT_PROTOCOL_UNKNOWN;
    }
    
    pthread_rwlock_rdlock(&registry->lock);
    
    int slot = find_slot_by_id(registry, client_id, false);
    if (slot < 0 || !registry->entries[slot].active) {
        pthread_rwlock_unlock(&registry->lock);
        return CLIENT_PROTOCOL_UNKNOWN;
    }
    
    client_protocol_t protocol = registry->entries[slot].protocol;
    
    pthread_rwlock_unlock(&registry->lock);
    
    return protocol;
}

transport_type_t client_registry_get_transport(client_registry_t* registry,
                                                uint32_t client_id) {
    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return TRANSPORT_UNKNOWN;
    }
    
    pthread_rwlock_rdlock(&registry->lock);
    
    int slot = find_slot_by_id(registry, client_id, false);
    if (slot < 0 || !registry->entries[slot].active) {
        pthread_rwlock_unlock(&registry->lock);
        return TRANSPORT_UNKNOWN;
    }
    
    transport_type_t transport = registry->entries[slot].transport;
    
    pthread_rwlock_unlock(&registry->lock);
    
    return transport;
}

/* ============================================================================
 * Client State Updates
 * ============================================================================ */

bool client_registry_set_protocol(client_registry_t* registry,
                                   uint32_t client_id,
                                   client_protocol_t protocol) {
    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return false;
    }
    
    pthread_rwlock_wrlock(&registry->lock);
    
    int slot = find_slot_by_id(registry, client_id, false);
    if (slot < 0 || !registry->entries[slot].active) {
        pthread_rwlock_unlock(&registry->lock);
        return false;
    }
    
    /* Only set if currently unknown */
    if (registry->entries[slot].protocol == CLIENT_PROTOCOL_UNKNOWN) {
        registry->entries[slot].protocol = protocol;
    }
    
    pthread_rwlock_unlock(&registry->lock);
    
    return true;
}

void client_registry_touch(client_registry_t* registry, uint32_t client_id) {
    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return;
    }
    
    pthread_rwlock_rdlock(&registry->lock);
    
    int slot = find_slot_by_id(registry, client_id, false);
    if (slot >= 0 && registry->entries[slot].active) {
        registry->entries[slot].last_seen = get_timestamp_ns();
    }
    
    pthread_rwlock_unlock(&registry->lock);
}

void client_registry_inc_received(client_registry_t* registry, uint32_t client_id) {
    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return;
    }
    
    pthread_rwlock_rdlock(&registry->lock);
    
    int slot = find_slot_by_id(registry, client_id, false);
    if (slot >= 0 && registry->entries[slot].active) {
        atomic_fetch_add(&registry->entries[slot].messages_received, 1);
    }
    
    pthread_rwlock_unlock(&registry->lock);
}

void client_registry_inc_sent(client_registry_t* registry, uint32_t client_id) {
    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return;
    }
    
    pthread_rwlock_rdlock(&registry->lock);
    
    int slot = find_slot_by_id(registry, client_id, false);
    if (slot >= 0 && registry->entries[slot].active) {
        atomic_fetch_add(&registry->entries[slot].messages_sent, 1);
    }
    
    pthread_rwlock_unlock(&registry->lock);
}

/* ============================================================================
 * Iteration (for broadcast)
 * ============================================================================ */

uint32_t client_registry_foreach(client_registry_t* registry,
                                  client_iterator_fn callback,
                                  void* user_data) {
    uint32_t count = 0;
    
    pthread_rwlock_rdlock(&registry->lock);
    
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE; i++) {
        client_entry_t* entry = &registry->entries[i];
        if (entry->active) {
            count++;
            if (!callback(entry, user_data)) {
                break;
            }
        }
    }
    
    pthread_rwlock_unlock(&registry->lock);
    
    return count;
}

uint32_t client_registry_foreach_tcp(client_registry_t* registry,
                                      client_iterator_fn callback,
                                      void* user_data) {
    uint32_t count = 0;
    
    pthread_rwlock_rdlock(&registry->lock);
    
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE; i++) {
        client_entry_t* entry = &registry->entries[i];
        if (entry->active && entry->transport == TRANSPORT_TCP) {
            count++;
            if (!callback(entry, user_data)) {
                break;
            }
        }
    }
    
    pthread_rwlock_unlock(&registry->lock);
    
    return count;
}

uint32_t client_registry_foreach_udp(client_registry_t* registry,
                                      client_iterator_fn callback,
                                      void* user_data) {
    uint32_t count = 0;
    
    pthread_rwlock_rdlock(&registry->lock);
    
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE; i++) {
        client_entry_t* entry = &registry->entries[i];
        if (entry->active && entry->transport == TRANSPORT_UDP) {
            count++;
            if (!callback(entry, user_data)) {
                break;
            }
        }
    }
    
    pthread_rwlock_unlock(&registry->lock);
    
    return count;
}

uint32_t client_registry_get_all_ids(client_registry_t* registry,
                                      uint32_t* out_ids,
                                      uint32_t max_ids) {
    uint32_t count = 0;
    
    pthread_rwlock_rdlock(&registry->lock);
    
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE && count < max_ids; i++) {
        client_entry_t* entry = &registry->entries[i];
        if (entry->active) {
            out_ids[count++] = entry->client_id;
        }
    }
    
    pthread_rwlock_unlock(&registry->lock);
    
    return count;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void client_registry_get_stats(client_registry_t* registry,
                                client_registry_stats_t* out_stats) {
    out_stats->tcp_clients_active = atomic_load(&registry->tcp_client_count);
    out_stats->udp_clients_active = atomic_load(&registry->udp_client_count);
    out_stats->tcp_connections_total = atomic_load(&registry->total_tcp_connections);
    out_stats->udp_connections_total = atomic_load(&registry->total_udp_connections);
}

void client_registry_print_stats(client_registry_t* registry) {
    client_registry_stats_t stats;
    client_registry_get_stats(registry, &stats);
    
    fprintf(stderr, "\n=== Client Registry Statistics ===\n");
    fprintf(stderr, "TCP clients active:     %u\n", stats.tcp_clients_active);
    fprintf(stderr, "UDP clients active:     %u\n", stats.udp_clients_active);
    fprintf(stderr, "Total clients active:   %u\n", 
            stats.tcp_clients_active + stats.udp_clients_active);
    fprintf(stderr, "TCP connections total:  %llu\n", 
            (unsigned long long)stats.tcp_connections_total);
    fprintf(stderr, "UDP connections total:  %llu\n", 
            (unsigned long long)stats.udp_connections_total);
}

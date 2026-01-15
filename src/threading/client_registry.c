#include "threading/client_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Maximum probe length for hash table lookups (Rule 2 compliance) */
#define MAX_PROBE_LENGTH 256

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * Get current timestamp in nanoseconds
 * Rule 7: Check clock_gettime return value
 */
static inline int64_t get_timestamp_ns(void) {
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0 && "clock_gettime failed");
    (void)rc;  /* Suppress unused warning in release */
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/**
 * Hash function for client_id
 * Uses FNV-1a with byte-wise mixing for good distribution
 */
static inline uint32_t hash_client_id(uint32_t client_id) {
    uint32_t hash = 2166136261u;  /* FNV offset basis */
    hash ^= (client_id & 0xFF);
    hash *= 16777619u;  /* FNV prime */
    hash ^= ((client_id >> 8) & 0xFF);
    hash *= 16777619u;
    hash ^= ((client_id >> 16) & 0xFF);
    hash *= 16777619u;
    hash ^= ((client_id >> 24) & 0xFF);
    hash *= 16777619u;
    return hash;
}

/**
 * Safe snapshot copy of a registry entry into out_entry.
 *
 * IMPORTANT:
 * - Do NOT raw-copy the struct because it contains atomics.
 * - Atomics must be read via atomic_load to avoid tearing/TSAN races,
 *   especially under -O3 where the compiler may vectorize copies.
 *
 * Caller must hold registry lock (read or write) while calling.
 */
static inline void snapshot_entry(const client_entry_t* src, client_entry_t* dst) {
    assert(src != NULL && "NULL src in snapshot_entry");
    assert(dst != NULL && "NULL dst in snapshot_entry");

    /* Copy stable, non-atomic fields */
    dst->last_seen  = src->last_seen;
    dst->handle     = src->handle;
    dst->client_id  = src->client_id;
    dst->transport  = src->transport;
    dst->protocol   = src->protocol;
    dst->active     = src->active;

    /* Copy atomic counters using atomic_load (relaxed is fine for stats) */
    atomic_store_explicit(&dst->messages_sent,
                          atomic_load_explicit(&src->messages_sent, memory_order_relaxed),
                          memory_order_relaxed);

    atomic_store_explicit(&dst->messages_received,
                          atomic_load_explicit(&src->messages_received, memory_order_relaxed),
                          memory_order_relaxed);

    /* Keep padding deterministic */
    memset(dst->_pad, 0, sizeof(dst->_pad));
}

/**
 * Find slot for client_id using linear probing
 *
 * @param registry    Client registry
 * @param client_id   Client ID to find
 * @param find_empty  If true, return first empty slot; if false, return matching slot
 * @return            Slot index, or -1 if not found
 */
static int find_slot_by_id(client_registry_t* registry, uint32_t client_id, bool find_empty) {
    assert(registry != NULL && "NULL registry in find_slot_by_id");
    assert(client_id != 0 && "Invalid client_id 0 in find_slot_by_id");

    uint32_t hash = hash_client_id(client_id);
    uint32_t start = hash % CLIENT_REGISTRY_HASH_SIZE;

    /* Rule 2: Bounded loop with explicit limit */
    uint32_t max_probes = CLIENT_REGISTRY_HASH_SIZE;
    if (max_probes > MAX_PROBE_LENGTH) {
        max_probes = MAX_PROBE_LENGTH;
    }

    for (uint32_t i = 0; i < max_probes; i++) {
        uint32_t idx = (start + i) % CLIENT_REGISTRY_HASH_SIZE;
        client_entry_t* entry = &registry->entries[idx];

        if (!entry->active) {
            if (find_empty) {
                return (int)idx;
            }
            /* NOTE: We continue probing because we do not maintain tombstones.
             * This avoids false negatives after deletions at the cost of extra probes.
             */
            continue;
        }

        if (entry->client_id == client_id) {
            return (int)idx;
        }
    }

    return -1;
}

/**
 * Find slot for UDP address (linear scan)
 *
 * NOTE: This is O(n) - acceptable for small client counts.
 * For high client counts, consider a separate UDP address hash table.
 */
static int find_slot_by_udp_addr(client_registry_t* registry, udp_client_addr_t addr) {
    assert(registry != NULL && "NULL registry in find_slot_by_udp_addr");

    /* Rule 2: Bounded by CLIENT_REGISTRY_HASH_SIZE */
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
 * Find an empty slot using hash as hint
 */
static int find_empty_slot(client_registry_t* registry, uint32_t hint_id) {
    assert(registry != NULL && "NULL registry in find_empty_slot");

    uint32_t hash = hash_client_id(hint_id);
    uint32_t start = hash % CLIENT_REGISTRY_HASH_SIZE;

    /* Rule 2: Bounded loop */
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
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_init");

    memset(registry->entries, 0, sizeof(registry->entries));

    /* TCP IDs: 1 to 0x7FFFFFFF */
    atomic_store(&registry->next_tcp_id, 1);

    /* UDP IDs: 0x80000001 to 0xFFFFFFFF */
    atomic_store(&registry->next_udp_id, CLIENT_ID_UDP_BASE + 1);

    atomic_store(&registry->tcp_client_count, 0);
    atomic_store(&registry->udp_client_count, 0);
    atomic_store(&registry->total_tcp_connections, 0);
    atomic_store(&registry->total_udp_connections, 0);

    int rc = pthread_rwlock_init(&registry->lock, NULL);
    assert(rc == 0 && "pthread_rwlock_init failed");
    (void)rc;

    /* Rule 5: Postcondition */
    assert(atomic_load(&registry->tcp_client_count) == 0 && "tcp_client_count not zero after init");
}

void client_registry_destroy(client_registry_t* registry) {
    /* Rule 5: Preconditions */
    assert(registry != NULL && "NULL registry in client_registry_destroy");

    int rc = pthread_rwlock_destroy(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_destroy failed");
    (void)rc;
}

/* ============================================================================
 * Client Registration
 * ============================================================================ */

uint32_t client_registry_add_tcp(client_registry_t* registry, int tcp_fd) {
    /* Rule 5: Preconditions */
    assert(registry != NULL && "NULL registry in client_registry_add_tcp");
    assert(tcp_fd >= 0 && "Invalid tcp_fd");

    /* Generate new TCP client ID */
    uint32_t client_id = atomic_fetch_add(&registry->next_tcp_id, 1);

    /* Wrap around if needed (skip 0 and UDP range) */
    if (client_id >= CLIENT_ID_UDP_BASE) {
        client_id = 1;
        atomic_store(&registry->next_tcp_id, 2);
    }

    int rc = pthread_rwlock_wrlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_wrlock failed");

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

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return client_id;
}

uint32_t client_registry_add_udp(client_registry_t* registry, udp_client_addr_t addr) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_add_udp");

    /* Generate new UDP client ID */
    uint32_t client_id = atomic_fetch_add(&registry->next_udp_id, 1);

    /* Wrap around if needed */
    if (client_id == 0) {
        client_id = CLIENT_ID_UDP_BASE + 1;
        atomic_store(&registry->next_udp_id, CLIENT_ID_UDP_BASE + 2);
    }

    int rc = pthread_rwlock_wrlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_wrlock failed");

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

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return client_id;
}

uint32_t client_registry_get_or_add_udp(client_registry_t* registry, udp_client_addr_t addr) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_get_or_add_udp");

    /* First try read lock to find existing */
    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    int slot = find_slot_by_udp_addr(registry, addr);
    if (slot >= 0) {
        uint32_t client_id = registry->entries[slot].client_id;
        pthread_rwlock_unlock(&registry->lock);
        return client_id;
    }

    pthread_rwlock_unlock(&registry->lock);

    /* Not found - need to add with write lock */
    rc = pthread_rwlock_wrlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_wrlock failed");

    /* Re-check under write lock (double-checked locking) */
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

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return client_id;
}

bool client_registry_remove(client_registry_t* registry, uint32_t client_id) {
    /* Rule 5: Preconditions */
    assert(registry != NULL && "NULL registry in client_registry_remove");

    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return false;
    }

    int rc = pthread_rwlock_wrlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_wrlock failed");

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

    /* Clear entry deterministically */
    entry->active = false;
    entry->client_id = 0;
    entry->transport = TRANSPORT_UNKNOWN;
    entry->protocol = CLIENT_PROTOCOL_UNKNOWN;
    entry->handle.tcp_fd = -1;
    entry->last_seen = 0;
    atomic_store(&entry->messages_sent, 0);
    atomic_store(&entry->messages_received, 0);
    memset(entry->_pad, 0, sizeof(entry->_pad));

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return true;
}

/* ============================================================================
 * Client Lookup
 * ============================================================================ */

bool client_registry_find(client_registry_t* registry, uint32_t client_id,
                          client_entry_t* out_entry) {
    /* Rule 5: Preconditions */
    assert(registry != NULL && "NULL registry in client_registry_find");

    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return false;
    }

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    int slot = find_slot_by_id(registry, client_id, false);
    if (slot < 0 || !registry->entries[slot].active) {
        pthread_rwlock_unlock(&registry->lock);
        return false;
    }

    if (out_entry) {
        /* SAFE snapshot copy (no raw struct assignment over atomics) */
        snapshot_entry(&registry->entries[slot], out_entry);
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return true;
}

bool client_registry_find_udp_by_addr(client_registry_t* registry,
                                       udp_client_addr_t addr,
                                       uint32_t* out_id) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_find_udp_by_addr");

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    int slot = find_slot_by_udp_addr(registry, addr);
    if (slot < 0) {
        pthread_rwlock_unlock(&registry->lock);
        return false;
    }

    if (out_id) {
        *out_id = registry->entries[slot].client_id;
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return true;
}

client_protocol_t client_registry_get_protocol(client_registry_t* registry,
                                                uint32_t client_id) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_get_protocol");

    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return CLIENT_PROTOCOL_UNKNOWN;
    }

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    int slot = find_slot_by_id(registry, client_id, false);
    if (slot < 0 || !registry->entries[slot].active) {
        pthread_rwlock_unlock(&registry->lock);
        return CLIENT_PROTOCOL_UNKNOWN;
    }

    client_protocol_t protocol = registry->entries[slot].protocol;

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return protocol;
}

transport_type_t client_registry_get_transport(client_registry_t* registry,
                                                uint32_t client_id) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_get_transport");

    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return TRANSPORT_UNKNOWN;
    }

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    int slot = find_slot_by_id(registry, client_id, false);
    if (slot < 0 || !registry->entries[slot].active) {
        pthread_rwlock_unlock(&registry->lock);
        return TRANSPORT_UNKNOWN;
    }

    transport_type_t transport = registry->entries[slot].transport;

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return transport;
}

/* ============================================================================
 * Client State Updates
 * ============================================================================ */

bool client_registry_set_protocol(client_registry_t* registry,
                                   uint32_t client_id,
                                   client_protocol_t protocol) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_set_protocol");

    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return false;
    }

    int rc = pthread_rwlock_wrlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_wrlock failed");

    int slot = find_slot_by_id(registry, client_id, false);
    if (slot < 0 || !registry->entries[slot].active) {
        pthread_rwlock_unlock(&registry->lock);
        return false;
    }

    /* Only set if currently unknown */
    if (registry->entries[slot].protocol == CLIENT_PROTOCOL_UNKNOWN) {
        registry->entries[slot].protocol = protocol;
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return true;
}

void client_registry_touch(client_registry_t* registry, uint32_t client_id) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_touch");

    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return;
    }

    /* FIX: Must not write under read lock (was UB, and can crash under -O3). */
    int rc = pthread_rwlock_wrlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_wrlock failed");

    int slot = find_slot_by_id(registry, client_id, false);
    if (slot >= 0 && registry->entries[slot].active) {
        registry->entries[slot].last_seen = get_timestamp_ns();
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;
}

void client_registry_inc_received(client_registry_t* registry, uint32_t client_id) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_inc_received");

    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return;
    }

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    int slot = find_slot_by_id(registry, client_id, false);
    if (slot >= 0 && registry->entries[slot].active) {
        atomic_fetch_add_explicit(&registry->entries[slot].messages_received, 1,
                                 memory_order_relaxed);
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;
}

void client_registry_inc_sent(client_registry_t* registry, uint32_t client_id) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_inc_sent");

    if (client_id == 0 || client_id == CLIENT_ID_BROADCAST) {
        return;
    }

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    int slot = find_slot_by_id(registry, client_id, false);
    if (slot >= 0 && registry->entries[slot].active) {
        atomic_fetch_add_explicit(&registry->entries[slot].messages_sent, 1,
                                 memory_order_relaxed);
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;
}

/* ============================================================================
 * Iteration (for broadcast)
 * ============================================================================ */

uint32_t client_registry_foreach(client_registry_t* registry,
                                  client_iterator_fn callback,
                                  void* user_data) {
    /* Rule 5: Preconditions */
    assert(registry != NULL && "NULL registry in client_registry_foreach");
    assert(callback != NULL && "NULL callback in client_registry_foreach");

    uint32_t count = 0;

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    /* Rule 2: Bounded by CLIENT_REGISTRY_HASH_SIZE */
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE; i++) {
        client_entry_t* entry = &registry->entries[i];
        if (entry->active) {
            count++;
            if (!callback(entry, user_data)) {
                break;
            }
        }
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return count;
}

uint32_t client_registry_foreach_tcp(client_registry_t* registry,
                                      client_iterator_fn callback,
                                      void* user_data) {
    /* Rule 5: Preconditions */
    assert(registry != NULL && "NULL registry in client_registry_foreach_tcp");
    assert(callback != NULL && "NULL callback in client_registry_foreach_tcp");

    uint32_t count = 0;

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    /* Rule 2: Bounded by CLIENT_REGISTRY_HASH_SIZE */
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE; i++) {
        client_entry_t* entry = &registry->entries[i];
        if (entry->active && entry->transport == TRANSPORT_TCP) {
            count++;
            if (!callback(entry, user_data)) {
                break;
            }
        }
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return count;
}

uint32_t client_registry_foreach_udp(client_registry_t* registry,
                                      client_iterator_fn callback,
                                      void* user_data) {
    /* Rule 5: Preconditions */
    assert(registry != NULL && "NULL registry in client_registry_foreach_udp");
    assert(callback != NULL && "NULL callback in client_registry_foreach_udp");

    uint32_t count = 0;

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    /* Rule 2: Bounded by CLIENT_REGISTRY_HASH_SIZE */
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE; i++) {
        client_entry_t* entry = &registry->entries[i];
        if (entry->active && entry->transport == TRANSPORT_UDP) {
            count++;
            if (!callback(entry, user_data)) {
                break;
            }
        }
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return count;
}

uint32_t client_registry_get_all_ids(client_registry_t* registry,
                                      uint32_t* out_ids,
                                      uint32_t max_ids) {
    /* Rule 5: Preconditions */
    assert(registry != NULL && "NULL registry in client_registry_get_all_ids");
    assert(out_ids != NULL && "NULL out_ids in client_registry_get_all_ids");
    assert(max_ids > 0 && "Zero max_ids in client_registry_get_all_ids");

    uint32_t count = 0;

    int rc = pthread_rwlock_rdlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_rdlock failed");

    /* Rule 2: Bounded by CLIENT_REGISTRY_HASH_SIZE and max_ids */
    for (uint32_t i = 0; i < CLIENT_REGISTRY_HASH_SIZE && count < max_ids; i++) {
        client_entry_t* entry = &registry->entries[i];
        if (entry->active) {
            out_ids[count++] = entry->client_id;
        }
    }

    rc = pthread_rwlock_unlock(&registry->lock);
    assert(rc == 0 && "pthread_rwlock_unlock failed");
    (void)rc;

    return count;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void client_registry_get_stats(client_registry_t* registry,
                                client_registry_stats_t* out_stats) {
    /* Rule 5: Preconditions */
    assert(registry != NULL && "NULL registry in client_registry_get_stats");
    assert(out_stats != NULL && "NULL out_stats in client_registry_get_stats");

    out_stats->tcp_clients_active = atomic_load(&registry->tcp_client_count);
    out_stats->udp_clients_active = atomic_load(&registry->udp_client_count);
    out_stats->tcp_connections_total = atomic_load(&registry->total_tcp_connections);
    out_stats->udp_connections_total = atomic_load(&registry->total_udp_connections);
}

void client_registry_print_stats(client_registry_t* registry) {
    /* Rule 5: Precondition */
    assert(registry != NULL && "NULL registry in client_registry_print_stats");

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


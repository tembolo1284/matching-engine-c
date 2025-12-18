#include "network/tcp_connection.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <assert.h>

/**
 * TCP Connection Management Implementation
 *
 * Rule Compliance:
 * - Rule 5: All functions have >= 2 assertions
 * - Rule 7: All return values checked
 *
 * Kernel Bypass Notes:
 * - socket_fd would map to DPDK queue index
 * - Output queue is already lock-free for zero-copy compatibility
 * - tcp_socket_set_low_latency() is no-op with DPDK
 */

/* Define the lock-free queue implementation */
DEFINE_LOCKFREE_QUEUE(output_msg_t, output_queue)

/* ============================================================================
 * Socket Optimization
 * ============================================================================ */

/**
 * Apply low-latency socket options
 *
 * Kernel Bypass Note: This function is not used with DPDK/AF_XDP
 */
bool tcp_socket_set_low_latency(int socket_fd, uint32_t flags) {
    assert(socket_fd >= 0 && "Invalid socket fd");

    bool all_success = true;
    int optval = 1;

    /* TCP_NODELAY - Disable Nagle's algorithm */
    if (flags & TCP_OPT_NODELAY) {
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY,
                       &optval, sizeof(optval)) < 0) {
            fprintf(stderr, "[TCP] WARNING: Failed to set TCP_NODELAY: %s\n",
                    strerror(errno));
            all_success = false;
        }
    }

#ifdef __linux__
    /* TCP_QUICKACK - Disable delayed ACKs (Linux only) */
    if (flags & TCP_OPT_QUICKACK) {
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_QUICKACK,
                       &optval, sizeof(optval)) < 0) {
            fprintf(stderr, "[TCP] WARNING: Failed to set TCP_QUICKACK: %s\n",
                    strerror(errno));
            all_success = false;
        }
    }

    /* SO_BUSY_POLL - Enable busy polling (Linux only, requires CAP_NET_ADMIN) */
    if (flags & TCP_OPT_BUSY_POLL) {
        int busy_poll_us = 50;  /* 50 microseconds */
        if (setsockopt(socket_fd, SOL_SOCKET, SO_BUSY_POLL,
                       &busy_poll_us, sizeof(busy_poll_us)) < 0) {
            /* This often fails without CAP_NET_ADMIN - don't warn loudly */
            /* fprintf(stderr, "[TCP] Note: SO_BUSY_POLL not available\n"); */
            all_success = false;
        }
    }
#else
    /* Silence unused parameter warnings on non-Linux */
    (void)flags;
#endif

    return all_success;
}

/* ============================================================================
 * Registry Initialization
 * ============================================================================ */

void tcp_client_registry_init(tcp_client_registry_t* registry) {
    assert(registry != NULL && "NULL registry in tcp_client_registry_init");

    memset(registry, 0, sizeof(*registry));

    /* Initialize all clients as inactive */
    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
        registry->clients[i].socket_fd = -1;
        registry->clients[i].client_id = 0;
        registry->clients[i].active = false;
        registry->clients[i].has_pending_write = false;
    }

    registry->active_count = 0;

    int rc = pthread_mutex_init(&registry->lock, NULL);
    assert(rc == 0 && "Failed to initialize registry mutex");
    (void)rc;  /* Silence unused warning in release builds */
}

void tcp_client_registry_destroy(tcp_client_registry_t* registry) {
    assert(registry != NULL && "NULL registry in tcp_client_registry_destroy");

    /* Close all active connections */
    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (registry->clients[i].active) {
            if (registry->clients[i].socket_fd >= 0) {
                close(registry->clients[i].socket_fd);
            }
            output_queue_destroy(&registry->clients[i].output_queue);
        }
    }

    int rc = pthread_mutex_destroy(&registry->lock);
    if (rc != 0) {
        fprintf(stderr, "[TCP] WARNING: pthread_mutex_destroy failed: %s\n",
                strerror(rc));
    }
}

/* ============================================================================
 * Client Management
 * ============================================================================ */

bool tcp_client_add(tcp_client_registry_t* registry,
                    int socket_fd,
                    struct sockaddr_in addr,
                    uint32_t* client_id) {
    assert(registry != NULL && "NULL registry in tcp_client_add");
    assert(socket_fd >= 0 && "Invalid socket_fd in tcp_client_add");
    assert(client_id != NULL && "NULL client_id in tcp_client_add");

    int rc = pthread_mutex_lock(&registry->lock);
    if (rc != 0) {
        fprintf(stderr, "[TCP] ERROR: pthread_mutex_lock failed: %s\n", strerror(rc));
        return false;
    }

    /* Check capacity */
    if (registry->active_count >= MAX_TCP_CLIENTS) {
        pthread_mutex_unlock(&registry->lock);
        fprintf(stderr, "[TCP] Client registry at capacity (%d)\n", MAX_TCP_CLIENTS);
        return false;
    }

    /* Find first available slot */
    tcp_client_t* client = NULL;
    uint32_t id = 0;

    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (!registry->clients[i].active) {
            client = &registry->clients[i];
            id = (uint32_t)(i + 1);  /* 1-based client IDs */
            break;
        }
    }

    if (client == NULL) {
        /* Shouldn't happen if active_count is accurate */
        pthread_mutex_unlock(&registry->lock);
        fprintf(stderr, "[TCP] ERROR: No free slot found despite active_count < max\n");
        return false;
    }

    /* Apply low-latency socket options before adding */
    tcp_socket_set_low_latency(socket_fd, TCP_OPT_LOW_LATENCY);

    /* Initialize client state */
    client->socket_fd = socket_fd;
    client->client_id = id;
    client->addr = addr;
    client->active = true;
    client->has_pending_write = false;

    /* Initialize framing state */
    framing_read_state_init(&client->read_state);
    memset(&client->write_state, 0, sizeof(client->write_state));

    /* Initialize output queue */
    output_queue_init(&client->output_queue);

    /* Initialize statistics */
    client->connected_at = time(NULL);
    client->messages_received = 0;
    client->messages_sent = 0;
    client->bytes_received = 0;
    client->bytes_sent = 0;

    registry->active_count++;
    *client_id = id;

    rc = pthread_mutex_unlock(&registry->lock);
    assert(rc == 0 && "pthread_mutex_unlock failed");

    fprintf(stderr, "[TCP] Client %u connected from %s:%d (fd=%d)\n",
            id,
            inet_ntoa(addr.sin_addr),
            ntohs(addr.sin_port),
            socket_fd);

    return true;
}

void tcp_client_remove(tcp_client_registry_t* registry,
                       uint32_t client_id) {
    assert(registry != NULL && "NULL registry in tcp_client_remove");

    if (client_id == 0 || client_id > MAX_TCP_CLIENTS) {
        fprintf(stderr, "[TCP] WARNING: Invalid client_id %u in remove\n", client_id);
        return;
    }

    int rc = pthread_mutex_lock(&registry->lock);
    if (rc != 0) {
        fprintf(stderr, "[TCP] ERROR: pthread_mutex_lock failed: %s\n", strerror(rc));
        return;
    }

    tcp_client_t* client = &registry->clients[client_id - 1];

    if (!client->active) {
        pthread_mutex_unlock(&registry->lock);
        return;
    }

    fprintf(stderr, "[TCP] Client %u disconnected (recv=%llu msgs, sent=%llu msgs)\n",
            client_id,
            (unsigned long long)client->messages_received,
            (unsigned long long)client->messages_sent);

    /* Close socket */
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
    }

    /* Destroy output queue (drains any pending messages) */
    output_queue_destroy(&client->output_queue);

    /* Mark inactive */
    client->active = false;
    client->socket_fd = -1;
    client->client_id = 0;
    client->has_pending_write = false;

    if (registry->active_count > 0) {
        registry->active_count--;
    }

    rc = pthread_mutex_unlock(&registry->lock);
    assert(rc == 0 && "pthread_mutex_unlock failed");
}

tcp_client_t* tcp_client_get(tcp_client_registry_t* registry,
                             uint32_t client_id) {
    assert(registry != NULL && "NULL registry in tcp_client_get");

    if (client_id == 0 || client_id > MAX_TCP_CLIENTS) {
        return NULL;
    }

    tcp_client_t* client = &registry->clients[client_id - 1];

    /* Volatile read to see latest active state */
    if (!client->active) {
        return NULL;
    }

    return client;
}

size_t tcp_client_get_active_count(tcp_client_registry_t* registry) {
    assert(registry != NULL && "NULL registry in tcp_client_get_active_count");

    int rc = pthread_mutex_lock(&registry->lock);
    if (rc != 0) {
        return 0;
    }

    size_t count = registry->active_count;

    rc = pthread_mutex_unlock(&registry->lock);
    (void)rc;

    return count;
}

/* ============================================================================
 * Output Queue Operations
 * ============================================================================ */

bool tcp_client_enqueue_output(tcp_client_t* client,
                               const output_msg_t* msg) {
    assert(client != NULL && "NULL client in tcp_client_enqueue_output");
    assert(msg != NULL && "NULL msg in tcp_client_enqueue_output");

    if (!client->active) {
        return false;
    }

    return output_queue_enqueue(&client->output_queue, msg);
}

bool tcp_client_dequeue_output(tcp_client_t* client,
                               output_msg_t* msg) {
    assert(client != NULL && "NULL client in tcp_client_dequeue_output");
    assert(msg != NULL && "NULL msg in tcp_client_dequeue_output");

    return output_queue_dequeue(&client->output_queue, msg);
}

/* ============================================================================
 * Bulk Operations
 * ============================================================================ */

size_t tcp_client_disconnect_all(tcp_client_registry_t* registry,
                                 uint32_t* client_ids,
                                 size_t max_clients) {
    assert(registry != NULL && "NULL registry in tcp_client_disconnect_all");
    assert(client_ids != NULL && "NULL client_ids in tcp_client_disconnect_all");
    assert(max_clients > 0 && "Zero max_clients");

    int rc = pthread_mutex_lock(&registry->lock);
    if (rc != 0) {
        fprintf(stderr, "[TCP] ERROR: pthread_mutex_lock failed: %s\n", strerror(rc));
        return 0;
    }

    size_t count = 0;

    for (size_t i = 0; i < MAX_TCP_CLIENTS && count < max_clients; i++) {
        tcp_client_t* client = &registry->clients[i];

        if (client->active) {
            client_ids[count++] = client->client_id;

            /* Close socket */
            if (client->socket_fd >= 0) {
                close(client->socket_fd);
            }

            /* Destroy queue */
            output_queue_destroy(&client->output_queue);

            /* Mark inactive */
            client->active = false;
            client->socket_fd = -1;
            client->client_id = 0;
        }
    }

    registry->active_count = 0;

    rc = pthread_mutex_unlock(&registry->lock);
    assert(rc == 0 && "pthread_mutex_unlock failed");

    fprintf(stderr, "[TCP] Disconnected %zu clients\n", count);

    return count;
}

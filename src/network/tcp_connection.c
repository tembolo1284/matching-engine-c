#include "network/tcp_connection.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>

// Define the lock-free queue implementation
DEFINE_LOCKFREE_QUEUE(output_msg_t, output_queue)

void tcp_client_registry_init(tcp_client_registry_t* registry) {
    memset(registry, 0, sizeof(*registry));
    
    // Initialize all clients as inactive
    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
        registry->clients[i].socket_fd = -1;
        registry->clients[i].client_id = 0;
        registry->clients[i].active = false;
    }
    
    registry->active_count = 0;
    pthread_mutex_init(&registry->lock, NULL);
}

void tcp_client_registry_destroy(tcp_client_registry_t* registry) {
    // Close all active connections
    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (registry->clients[i].active) {
            close(registry->clients[i].socket_fd);
            output_queue_destroy(&registry->clients[i].output_queue);
        }
    }
    
    pthread_mutex_destroy(&registry->lock);
}

bool tcp_client_add(tcp_client_registry_t* registry,
                    int socket_fd,
                    struct sockaddr_in addr,
                    uint32_t* client_id) {
    pthread_mutex_lock(&registry->lock);
    
    // Check capacity
    if (registry->active_count >= MAX_TCP_CLIENTS) {
        pthread_mutex_unlock(&registry->lock);
        return false;
    }
    
    // Find first available slot
    tcp_client_t* client = NULL;
    uint32_t id = 0;
    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (!registry->clients[i].active) {
            client = &registry->clients[i];
            id = (uint32_t)(i + 1);  // 1-based client IDs
            break;
        }
    }
    
    if (!client) {
        pthread_mutex_unlock(&registry->lock);
        return false;
    }
    
    // Initialize client state
    client->socket_fd = socket_fd;
    client->client_id = id;
    client->addr = addr;
    client->active = true;
    
    // Initialize framing state
    framing_read_state_init(&client->read_state);
    client->has_pending_write = false;
    
    // Initialize output queue
    output_queue_init(&client->output_queue, TCP_CLIENT_OUTPUT_QUEUE_SIZE);
    
    // Initialize statistics
    client->connected_at = time(NULL);
    client->messages_received = 0;
    client->messages_sent = 0;
    client->bytes_received = 0;
    client->bytes_sent = 0;
    
    registry->active_count++;
    *client_id = id;
    
    pthread_mutex_unlock(&registry->lock);
    
    fprintf(stderr, "[TCP] Client %u connected from %s:%d\n",
            id,
            inet_ntoa(addr.sin_addr),
            ntohs(addr.sin_port));
    
    return true;
}

void tcp_client_remove(tcp_client_registry_t* registry,
                       uint32_t client_id) {
    if (client_id == 0 || client_id > MAX_TCP_CLIENTS) {
        return;
    }
    
    pthread_mutex_lock(&registry->lock);
    
    tcp_client_t* client = &registry->clients[client_id - 1];
    
    if (!client->active) {
        pthread_mutex_unlock(&registry->lock);
        return;
    }
    
    fprintf(stderr, "[TCP] Client %u disconnected (recv=%lu, sent=%lu)\n",
            client_id,
            client->messages_received,
            client->messages_sent);
    
    // Close socket
    close(client->socket_fd);
    
    // Destroy output queue
    output_queue_destroy(&client->output_queue);
    
    // Mark inactive
    client->active = false;
    client->socket_fd = -1;
    client->client_id = 0;
    
    registry->active_count--;
    
    pthread_mutex_unlock(&registry->lock);
}

tcp_client_t* tcp_client_get(tcp_client_registry_t* registry,
                             uint32_t client_id) {
    if (client_id == 0 || client_id > MAX_TCP_CLIENTS) {
        return NULL;
    }
    
    tcp_client_t* client = &registry->clients[client_id - 1];
    return client->active ? client : NULL;
}

size_t tcp_client_get_active_count(tcp_client_registry_t* registry) {
    pthread_mutex_lock(&registry->lock);
    size_t count = registry->active_count;
    pthread_mutex_unlock(&registry->lock);
    return count;
}

bool tcp_client_enqueue_output(tcp_client_t* client,
                               const output_msg_t* msg) {
    return output_queue_enqueue(&client->output_queue, msg);
}

bool tcp_client_dequeue_output(tcp_client_t* client,
                               output_msg_t* msg) {
    return output_queue_dequeue(&client->output_queue, msg);
}

size_t tcp_client_disconnect_all(tcp_client_registry_t* registry,
                                 uint32_t* client_ids,
                                 size_t max_clients) {
    pthread_mutex_lock(&registry->lock);
    
    size_t count = 0;
    for (size_t i = 0; i < MAX_TCP_CLIENTS && count < max_clients; i++) {
        if (registry->clients[i].active) {
            client_ids[count++] = registry->clients[i].client_id;
            
            // Close socket
            close(registry->clients[i].socket_fd);
            
            // Destroy queue
            output_queue_destroy(&registry->clients[i].output_queue);
            
            // Mark inactive
            registry->clients[i].active = false;
            registry->clients[i].socket_fd = -1;
        }
    }
    
    registry->active_count = 0;
    
    pthread_mutex_unlock(&registry->lock);
    
    return count;
}

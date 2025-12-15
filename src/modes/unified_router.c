#include "unified_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ============================================================================
 * Send to Multicast
 * ============================================================================ */

void unified_send_multicast(unified_server_t* server, const output_msg_t* msg) {
    if (server->multicast_fd < 0) return;
    
    size_t bin_len = 0;
    const void* bin_data = binary_message_formatter_format(&server->bin_formatter, msg, &bin_len);
    
    if (bin_data && bin_len > 0) {
        sendto(server->multicast_fd, bin_data, bin_len, 0,
               (struct sockaddr*)&server->multicast_addr, sizeof(server->multicast_addr));
        atomic_fetch_add(&server->multicast_messages, 1);
    }
}

/* ============================================================================
 * Send to Client Implementation
 * ============================================================================ */

bool unified_send_to_client(unified_server_t* server, 
                            uint32_t client_id,
                            const output_msg_t* msg) {
    if (client_id == 0) return false;
    
    client_entry_t entry;
    if (!client_registry_find(server->registry, client_id, &entry)) {
        return false;
    }
    
    /* Format message based on client protocol */
    const void* data;
    size_t len;
    
    if (entry.protocol == CLIENT_PROTOCOL_BINARY) {
        data = binary_message_formatter_format(&server->bin_formatter, msg, &len);
    } else {
        const char* line = message_formatter_format(&server->csv_formatter, msg);
        if (line) {
            data = line;
            len = strlen(line);
        } else {
            return false;
        }
    }
    
    if (!data || len == 0) return false;
    
    bool success = false;
    
    if (entry.transport == TRANSPORT_TCP) {
        /* TCP send - TODO: implement when we store fd in client entry */
        success = true;  /* Pretend success for now */
    } else if (entry.transport == TRANSPORT_UDP) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = entry.handle.udp_addr.addr;
        addr.sin_port = entry.handle.udp_addr.port;
        
        ssize_t sent = sendto(server->udp_fd, data, len, 0,
                              (struct sockaddr*)&addr, sizeof(addr));
        success = (sent == (ssize_t)len);
    }
    
    if (success) {
        client_registry_inc_sent(server->registry, client_id);
    }
    
    return success;
}

/* ============================================================================
 * Broadcast to All Clients
 * ============================================================================ */

void unified_broadcast_to_all(unified_server_t* server, const output_msg_t* msg) {
    /* Pre-format for both protocols */
    const char* csv_line = message_formatter_format(&server->csv_formatter, msg);
    size_t csv_len = csv_line ? strlen(csv_line) : 0;
    
    size_t bin_len = 0;
    const void* bin_data = binary_message_formatter_format(&server->bin_formatter, msg, &bin_len);
    
    /* Get all client IDs */
    uint32_t client_ids[MAX_REGISTERED_CLIENTS];
    uint32_t count = client_registry_get_all_ids(server->registry, client_ids, MAX_REGISTERED_CLIENTS);
    
    for (uint32_t i = 0; i < count; i++) {
        client_entry_t entry;
        if (!client_registry_find(server->registry, client_ids[i], &entry)) {
            continue;
        }
        
        const void* data;
        size_t len;
        
        if (entry.protocol == CLIENT_PROTOCOL_BINARY) {
            data = bin_data;
            len = bin_len;
        } else {
            data = csv_line;
            len = csv_len;
        }
        
        if (!data || len == 0) continue;
        
        if (entry.transport == TRANSPORT_UDP) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = entry.handle.udp_addr.addr;
            addr.sin_port = entry.handle.udp_addr.port;
            
            sendto(server->udp_fd, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
            client_registry_inc_sent(server->registry, client_ids[i]);
        }
        /* TCP: TODO */
    }
    
    atomic_fetch_add(&server->tob_broadcasts, 1);
}

/* ============================================================================
 * Process Single Output Envelope
 * ============================================================================ */

static void process_output_envelope(unified_server_t* server, 
                                    const output_msg_envelope_t* envelope) {
    const output_msg_t* msg = &envelope->msg;
    uint32_t originator = envelope->client_id;
    
    /* Route based on message type */
    switch (msg->type) {
        case OUTPUT_MSG_ACK:
        case OUTPUT_MSG_CANCEL_ACK:
            /* Send to originator only */
            unified_send_to_client(server, originator, msg);
            break;
            
        case OUTPUT_MSG_TRADE: {
            /* Send to both buyer and seller */
            /* Note: buyer/seller user_ids are in the trade message */
            uint32_t buyer_client = user_client_map_get(server->user_map,
                                                         msg->data.trade.user_id_buy);
            uint32_t seller_client = user_client_map_get(server->user_map,
                                                          msg->data.trade.user_id_sell);
            
            if (buyer_client != 0) {
                unified_send_to_client(server, buyer_client, msg);
            }
            if (seller_client != 0 && seller_client != buyer_client) {
                unified_send_to_client(server, seller_client, msg);
            }
            break;
        }
        
        case OUTPUT_MSG_TOP_OF_BOOK:
            /* Broadcast to all connected clients */
            unified_broadcast_to_all(server, msg);
            break;
            
        default:
            break;
    }
    
    /* Send to multicast (always binary) */
    unified_send_multicast(server, msg);
    
    atomic_fetch_add(&server->messages_routed, 1);
}

/* ============================================================================
 * Output Router Thread
 * ============================================================================ */

void* unified_output_router_thread(void* arg) {
    unified_server_t* server = (unified_server_t*)arg;
    
    fprintf(stderr, "[Router] Output router started\n");
    
    output_msg_envelope_t envelope;
    
    /* Progress tracking for quiet mode */
    uint64_t last_progress_time = unified_get_timestamp_ns();
    uint64_t start_time = last_progress_time;
    uint64_t last_routed = 0;
    const uint64_t PROGRESS_INTERVAL_NS = 10ULL * 1000000000ULL;  /* 10 seconds */
    
    while (!atomic_load(&g_shutdown)) {
        bool got_message = false;
        
        /* Drain output queue 0 */
        if (output_envelope_queue_dequeue(server->output_queue_0, &envelope)) {
            process_output_envelope(server, &envelope);
            got_message = true;
        }
        
        /* Drain output queue 1 (if dual processor) */
        if (server->output_queue_1 && 
            output_envelope_queue_dequeue(server->output_queue_1, &envelope)) {
            process_output_envelope(server, &envelope);
            got_message = true;
        }
        
        if (!got_message) {
            struct timespec ts = {0, 1000};  /* 1µs */
            nanosleep(&ts, NULL);
        }
        
        /* Progress update in quiet mode */
        if (server->config.quiet_mode) {
            uint64_t now = unified_get_timestamp_ns();
            if (now - last_progress_time >= PROGRESS_INTERVAL_NS) {
                uint64_t total_routed = atomic_load(&server->messages_routed);
                uint64_t elapsed_ns = now - start_time;
                double elapsed_sec = (double)elapsed_ns / 1e9;
                uint64_t interval_msgs = total_routed - last_routed;
                double interval_sec = (double)(now - last_progress_time) / 1e9;
                double current_rate = interval_sec > 0 ? interval_msgs / interval_sec : 0;
                double avg_rate = elapsed_sec > 0 ? total_routed / elapsed_sec : 0;
                
                client_registry_stats_t stats;
                client_registry_get_stats(server->registry, &stats);
                
                fprintf(stderr, "[PROGRESS] %6.1fs | %12llu routed | %8.2fK msg/s (avg: %.2fK) | TCP: %u UDP: %u\n",
                        elapsed_sec,
                        (unsigned long long)total_routed,
                        current_rate / 1000.0,
                        avg_rate / 1000.0,
                        stats.tcp_clients_active,
                        stats.udp_clients_active);
                
                last_progress_time = now;
                last_routed = total_routed;
            }
        }
    }
    
    fprintf(stderr, "[Router] Output router stopped\n");
    return NULL;
}

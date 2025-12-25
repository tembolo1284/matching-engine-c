#include "unified_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "protocol/csv/message_parser.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/binary/binary_protocol.h"

/* ============================================================================
 * TCP Client Handler Context
 * ============================================================================ */

typedef struct {
    unified_server_t* server;
    int client_fd;
    uint32_t client_id;
    struct sockaddr_in client_addr;
} tcp_client_ctx_t;

/* ============================================================================
 * Debug Helper - Hex Dump
 * ============================================================================ */

static void hex_dump(const char* prefix, const uint8_t* data, size_t len, size_t max_len) {
    // fprintf(stderr, "%s (%zu bytes): ", prefix, len);
    (void)prefix;
    size_t display = len < max_len ? len : max_len;
    // for (size_t i = 0; i < display; i++) {
    //     fprintf(stderr, "%02X ", data[i]);
    // }
    (void)display;
    (void) data;
    if (len > max_len) {
        // fprintf(stderr, "...");
    }
    // fprintf(stderr, "\n");
}

/* ============================================================================
 * Detect if client uses length-prefixed framing
 * ============================================================================ */

typedef enum {
    FRAMING_UNKNOWN,
    FRAMING_LENGTH_PREFIXED,  /* 4-byte big-endian length + message */
    FRAMING_RAW_BINARY,       /* Raw binary messages (magic byte first) */
    FRAMING_CSV               /* Newline-delimited CSV */
} framing_type_t;

static framing_type_t detect_framing(const uint8_t* data, size_t len) {
    if (len < 4) return FRAMING_UNKNOWN;
    
    /* Check for raw binary (starts with magic byte 0x4D) */
    if (data[0] == BINARY_MAGIC) {
        return FRAMING_RAW_BINARY;
    }
    
    /* Check for CSV (starts with N, C, or F) */
    if (data[0] == 'N' || data[0] == 'C' || data[0] == 'F') {
        return FRAMING_CSV;
    }
    
    /* Check for length-prefixed binary */
    /* First 4 bytes should be a reasonable length (e.g., < 1000) */
    /* And byte 4 should be the magic byte */
    uint32_t potential_len = ((uint32_t)data[0] << 24) |
                             ((uint32_t)data[1] << 16) |
                             ((uint32_t)data[2] << 8) |
                             ((uint32_t)data[3]);
    
    if (potential_len > 0 && potential_len < 1000) {
        /* Check if byte after length prefix is magic byte */
        if (len > 4 && data[4] == BINARY_MAGIC) {
            return FRAMING_LENGTH_PREFIXED;
        }
    }
    
    return FRAMING_UNKNOWN;
}

/* ============================================================================
 * TCP Client Handler Thread
 * ============================================================================ */

static void* tcp_client_handler(void* arg) {
    tcp_client_ctx_t* ctx = (tcp_client_ctx_t*)arg;
    unified_server_t* server = ctx->server;
    int fd = ctx->client_fd;
    uint32_t client_id = ctx->client_id;
    
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ctx->client_addr.sin_addr, addr_str, sizeof(addr_str));
    uint16_t port = ntohs(ctx->client_addr.sin_port);
    (void)port;  //remove when you bring the debug print back.
    // fprintf(stderr, "[TCP] Client %u connected from %s:%u (fd=%d)\n", 
            // client_id, addr_str, port, fd);
    
    /* Allocate receive buffer */
    uint8_t* recv_buffer = malloc(TCP_RECV_BUFFER_SIZE);
    if (!recv_buffer) {
        fprintf(stderr, "[TCP] Failed to allocate receive buffer for client %u\n", client_id);
        close(fd);
        client_registry_remove(server->registry, client_id);
        free(ctx);
        return NULL;
    }
    
    size_t buffer_used = 0;
    message_parser_t csv_parser;
    message_parser_init(&csv_parser);
    
    binary_message_parser_t bin_parser;
    binary_message_parser_init(&bin_parser);
    
    framing_type_t framing = FRAMING_UNKNOWN;
    client_protocol_t protocol = CLIENT_PROTOCOL_UNKNOWN;
    
    // fprintf(stderr, "[TCP] Client %u: Expected sizes: NewOrder=%zu Cancel=%zu Flush=%zu\n",
            // client_id, sizeof(binary_new_order_t), sizeof(binary_cancel_t), sizeof(binary_flush_t));
    
    while (!atomic_load(&g_shutdown)) {
        /* Receive data */
        ssize_t n = recv(fd, recv_buffer + buffer_used, 
                         TCP_RECV_BUFFER_SIZE - buffer_used, 0);
        
        if (n <= 0) {
            if (n == 0) {
                fprintf(stderr, "[TCP] Client %u: Connection closed by peer\n", client_id);
                break;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            } else {
                fprintf(stderr, "[TCP] Client %u: recv error: %s\n", client_id, strerror(errno));
                break;
            }
        }
        
        buffer_used += n;
        
        if (!server->config.quiet_mode) {
            // fprintf(stderr, "[TCP] Client %u: Received %zd bytes (buffer: %zu)\n", 
                    // client_id, n, buffer_used);
            hex_dump("[TCP] Data", recv_buffer, buffer_used, 48);
        }
        
        /* Detect framing on first data */
        if (framing == FRAMING_UNKNOWN && buffer_used >= 5) {
            framing = detect_framing(recv_buffer, buffer_used);
            
            const char* framing_str = 
                framing == FRAMING_LENGTH_PREFIXED ? "LENGTH_PREFIXED" :
                framing == FRAMING_RAW_BINARY ? "RAW_BINARY" :
                framing == FRAMING_CSV ? "CSV" : "UNKNOWN";
            
            fprintf(stderr, "[TCP] Client %u: Framing detected: %s\n", client_id, framing_str);
            
            if (framing == FRAMING_LENGTH_PREFIXED || framing == FRAMING_RAW_BINARY) {
                protocol = CLIENT_PROTOCOL_BINARY;
                client_registry_set_protocol(server->registry, client_id, protocol);
            } else if (framing == FRAMING_CSV) {
                protocol = CLIENT_PROTOCOL_CSV;
                client_registry_set_protocol(server->registry, client_id, protocol);
            }
        }
        
        /* Process messages based on framing type */
        size_t processed = 0;
        
        if (framing == FRAMING_LENGTH_PREFIXED) {
            /* Length-prefixed binary: [4-byte big-endian len][message] */
            while (processed + 4 <= buffer_used) {
                const uint8_t* ptr = recv_buffer + processed;
                
                /* Read length prefix (big-endian) */
                uint32_t msg_len = ((uint32_t)ptr[0] << 24) |
                                   ((uint32_t)ptr[1] << 16) |
                                   ((uint32_t)ptr[2] << 8) |
                                   ((uint32_t)ptr[3]);
                
                /* Sanity check */
                if (msg_len == 0 || msg_len > 10000) {
                    fprintf(stderr, "[TCP] Client %u: Invalid length %u\n", client_id, msg_len);
                    processed++;
                    continue;
                }
                
                /* Check if we have the full message */
                if (processed + 4 + msg_len > buffer_used) {
                    if (!server->config.quiet_mode) {
                        fprintf(stderr, "[TCP] Client %u: Incomplete msg, need %u more bytes\n",
                                client_id, (uint32_t)(processed + 4 + msg_len - buffer_used));
                    }
                    break;
                }
                
                /* Point to actual message (after length prefix) */
                const uint8_t* msg_start = ptr + 4;
                
                if (!server->config.quiet_mode) {
                    // fprintf(stderr, "[TCP] Client %u: Processing msg len=%u\n", client_id, msg_len);
                    hex_dump("[TCP] Msg", msg_start, msg_len, 32);
                }
                
                /* Verify magic byte */
                if (msg_start[0] != BINARY_MAGIC) {
                    fprintf(stderr, "[TCP] Client %u: Bad magic 0x%02X\n", client_id, msg_start[0]);
                    processed += 4 + msg_len;
                    continue;
                }
                
                /* Parse the binary message */
                input_msg_t input;
                if (binary_message_parser_parse(&bin_parser, msg_start, msg_len, &input)) {
                    if (!server->config.quiet_mode) {
                        // const char* type_str = 
                            // input.type == INPUT_MSG_NEW_ORDER ? "NEW_ORDER" :
                            // input.type == INPUT_MSG_CANCEL ? "CANCEL" :
                            // input.type == INPUT_MSG_FLUSH ? "FLUSH" : "UNKNOWN";
                        // fprintf(stderr, "[TCP] Client %u: Parsed %s\n", client_id, type_str);
                    }
                    
                    unified_route_input(server, &input, client_id, NULL);
                    atomic_fetch_add(&server->tcp_messages_received, 1);
                    client_registry_inc_received(server->registry, client_id);
                } else {
                    fprintf(stderr, "[TCP] Client %u: Parse failed\n", client_id);
                }
                
                processed += 4 + msg_len;
            }
        } else if (framing == FRAMING_RAW_BINARY) {
            /* Raw binary: messages start with magic byte, fixed sizes */
            while (processed < buffer_used) {
                size_t remaining = buffer_used - processed;
                const uint8_t* msg_start = recv_buffer + processed;
                
                if (remaining < 2) break;
                
                if (msg_start[0] != BINARY_MAGIC) {
                    processed++;
                    continue;
                }
                
                size_t msg_size = 0;
                switch (msg_start[1]) {
                    case 'N': msg_size = sizeof(binary_new_order_t); break;
                    case 'C': msg_size = sizeof(binary_cancel_t); break;
                    case 'F': msg_size = sizeof(binary_flush_t); break;
                    default:
                        processed++;
                        continue;
                }
                
                if (remaining < msg_size) break;
                
                input_msg_t input;
                if (binary_message_parser_parse(&bin_parser, msg_start, msg_size, &input)) {
                    unified_route_input(server, &input, client_id, NULL);
                    atomic_fetch_add(&server->tcp_messages_received, 1);
                    client_registry_inc_received(server->registry, client_id);
                }
                
                processed += msg_size;
            }
        } else if (framing == FRAMING_CSV) {
            /* CSV: newline-delimited */
            while (processed < buffer_used) {
                size_t line_end = processed;
                while (line_end < buffer_used && recv_buffer[line_end] != '\n') {
                    line_end++;
                }
                
                if (line_end >= buffer_used) break;
                
                recv_buffer[line_end] = '\0';
                const char* line = (const char*)(recv_buffer + processed);
                
                input_msg_t input;
                if (message_parser_parse(&csv_parser, line, &input)) {
                    unified_route_input(server, &input, client_id, NULL);
                    atomic_fetch_add(&server->tcp_messages_received, 1);
                    client_registry_inc_received(server->registry, client_id);
                }
                
                processed = line_end + 1;
            }
        }
        
        /* Compact buffer */
        if (processed > 0) {
            if (processed < buffer_used) {
                memmove(recv_buffer, recv_buffer + processed, buffer_used - processed);
            }
            buffer_used -= processed;
        }
    }
    
    fprintf(stderr, "[TCP] Client %u: Exiting handler\n", client_id);
    
    free(recv_buffer);
    close(fd);
    client_registry_remove(server->registry, client_id);
    free(ctx);
    
    return NULL;
}

/* ============================================================================
 * TCP Listener Thread
 * ============================================================================ */

void* unified_tcp_listener_thread(void* arg) {
    unified_server_t* server = (unified_server_t*)arg;
    
    fprintf(stderr, "[TCP] Listener started on port %u\n", server->config.tcp_port);
    
    while (!atomic_load(&g_shutdown)) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(server->tcp_listen_fd, 
                               (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            if (!atomic_load(&g_shutdown)) {
                fprintf(stderr, "[TCP] Accept error: %s\n", strerror(errno));
            }
            break;
        }
        
        /* Set TCP_NODELAY */
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        
        /* Set receive timeout */
        struct timeval tv = {5, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        /* Register client */
        uint32_t client_id = client_registry_add_tcp(server->registry, client_fd);
        if (client_id == 0) {
            fprintf(stderr, "[TCP] Failed to register client\n");
            close(client_fd);
            continue;
        }
        
        /* Create handler context */
        tcp_client_ctx_t* ctx = malloc(sizeof(tcp_client_ctx_t));
        if (!ctx) {
            fprintf(stderr, "[TCP] Failed to allocate context\n");
            close(client_fd);
            client_registry_remove(server->registry, client_id);
            continue;
        }
        
        ctx->server = server;
        ctx->client_fd = client_fd;
        ctx->client_id = client_id;
        ctx->client_addr = client_addr;
        
        /* Spawn handler thread */
        pthread_t handler_tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        
        if (pthread_create(&handler_tid, &attr, tcp_client_handler, ctx) != 0) {
            fprintf(stderr, "[TCP] Failed to create handler thread\n");
            close(client_fd);
            client_registry_remove(server->registry, client_id);
            free(ctx);
        }
        
        pthread_attr_destroy(&attr);
    }
    
    fprintf(stderr, "[TCP] Listener stopped\n");
    return NULL;
}

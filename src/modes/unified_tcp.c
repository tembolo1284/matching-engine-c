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
    
    if (!server->config.quiet_mode) {
        fprintf(stderr, "[TCP] Client %u connected from %s:%u (fd=%d)\n", 
                client_id, addr_str, port, fd);
    }
    
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
    
    client_protocol_t protocol = CLIENT_PROTOCOL_UNKNOWN;
    
    while (!atomic_load(&g_shutdown)) {
        /* Receive data */
        ssize_t n = recv(fd, recv_buffer + buffer_used, 
                         TCP_RECV_BUFFER_SIZE - buffer_used, 0);
        
        if (n <= 0) {
            if (n == 0) {
                if (!server->config.quiet_mode) {
                    fprintf(stderr, "[TCP] Client %u disconnected\n", client_id);
                }
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                if (!server->config.quiet_mode) {
                    fprintf(stderr, "[TCP] Client %u recv error: %s\n", 
                            client_id, strerror(errno));
                }
            }
            break;
        }
        
        buffer_used += n;
        
        if (!server->config.quiet_mode) {
            fprintf(stderr, "[TCP] Client %u received %zd bytes (buffer: %zu)\n", 
                    client_id, n, buffer_used);
        }
        
        /* Detect protocol on first data */
        if (protocol == CLIENT_PROTOCOL_UNKNOWN && buffer_used > 0) {
            protocol = unified_detect_protocol(recv_buffer, buffer_used);
            if (protocol != CLIENT_PROTOCOL_UNKNOWN) {
                client_registry_set_protocol(server->registry, client_id, protocol);
                if (!server->config.quiet_mode) {
                    fprintf(stderr, "[TCP] Client %u protocol detected: %s\n",
                            client_id, protocol == CLIENT_PROTOCOL_BINARY ? "BINARY" : "CSV");
                }
            }
        }
        
        /* Process messages based on protocol */
        size_t processed = 0;
        
        if (protocol == CLIENT_PROTOCOL_BINARY) {
            /* Binary protocol: fixed-size messages */
            while (processed < buffer_used) {
                size_t remaining = buffer_used - processed;
                const uint8_t* msg_start = recv_buffer + processed;
                
                if (remaining < 2) break;  /* Need at least magic + type */
                
                if (msg_start[0] != BINARY_MAGIC) {
                    if (!server->config.quiet_mode) {
                        fprintf(stderr, "[TCP] Client %u: bad magic byte 0x%02X at offset %zu\n",
                                client_id, msg_start[0], processed);
                    }
                    processed++;  /* Skip bad byte */
                    continue;
                }
                
                /* Determine message size */
                size_t msg_size = 0;
                uint8_t msg_type = msg_start[1];
                switch (msg_type) {
                    case 'N': msg_size = sizeof(binary_new_order_t); break;
                    case 'C': msg_size = sizeof(binary_cancel_t); break;
                    case 'F': msg_size = sizeof(binary_flush_t); break;
                    default:
                        if (!server->config.quiet_mode) {
                            fprintf(stderr, "[TCP] Client %u: unknown message type '%c' (0x%02X)\n",
                                    client_id, msg_type, msg_type);
                        }
                        processed++;
                        continue;
                }
                
                if (remaining < msg_size) {
                    if (!server->config.quiet_mode) {
                        fprintf(stderr, "[TCP] Client %u: incomplete message, need %zu have %zu\n",
                                client_id, msg_size, remaining);
                    }
                    break;
                }
                
                /* Parse binary message */
                input_msg_t input;
                if (binary_message_parser_parse(&bin_parser, msg_start, msg_size, &input)) {
                    if (!server->config.quiet_mode) {
                        fprintf(stderr, "[TCP] Client %u: parsed %s message\n",
                                client_id,
                                input.type == INPUT_MSG_NEW_ORDER ? "NEW_ORDER" :
                                input.type == INPUT_MSG_CANCEL ? "CANCEL" :
                                input.type == INPUT_MSG_FLUSH ? "FLUSH" : "UNKNOWN");
                    }
                    unified_route_input(server, &input, client_id, NULL);
                    atomic_fetch_add(&server->tcp_messages_received, 1);
                    client_registry_inc_received(server->registry, client_id);
                } else {
                    fprintf(stderr, "[TCP] Client %u: failed to parse binary message\n", client_id);
                }
                
                processed += msg_size;
            }
        } else {
            /* CSV protocol: newline-delimited */
            while (processed < buffer_used) {
                /* Find newline */
                size_t line_end = processed;
                while (line_end < buffer_used && recv_buffer[line_end] != '\n') {
                    line_end++;
                }
                
                if (line_end >= buffer_used) break;  /* No complete line */
                
                /* Null-terminate the line */
                recv_buffer[line_end] = '\0';
                const char* line = (const char*)(recv_buffer + processed);
                
                if (!server->config.quiet_mode) {
                    fprintf(stderr, "[TCP] Client %u: CSV line: '%s'\n", client_id, line);
                }
                
                /* Parse CSV message */
                input_msg_t input;
                if (message_parser_parse(&csv_parser, line, &input)) {
                    if (!server->config.quiet_mode) {
                        fprintf(stderr, "[TCP] Client %u: parsed %s message\n",
                                client_id,
                                input.type == INPUT_MSG_NEW_ORDER ? "NEW_ORDER" :
                                input.type == INPUT_MSG_CANCEL ? "CANCEL" :
                                input.type == INPUT_MSG_FLUSH ? "FLUSH" : "UNKNOWN");
                    }
                    unified_route_input(server, &input, client_id, NULL);
                    atomic_fetch_add(&server->tcp_messages_received, 1);
                    client_registry_inc_received(server->registry, client_id);
                } else {
                    fprintf(stderr, "[TCP] Client %u: failed to parse CSV line\n", client_id);
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
        
        /* Set non-blocking with timeout */
        struct timeval tv = {1, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        /* Register client WITH the file descriptor */
        uint32_t client_id = client_registry_add_tcp(server->registry, client_fd);
        if (client_id == 0) {
            fprintf(stderr, "[TCP] Failed to register client, closing connection\n");
            close(client_fd);
            continue;
        }
        
        /* Create client handler context */
        tcp_client_ctx_t* ctx = malloc(sizeof(tcp_client_ctx_t));
        if (!ctx) {
            fprintf(stderr, "[TCP] Failed to allocate client context\n");
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

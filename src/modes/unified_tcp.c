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
    fprintf(stderr, "%s (%zu bytes): ", prefix, len);
    size_t display = len < max_len ? len : max_len;
    for (size_t i = 0; i < display; i++) {
        fprintf(stderr, "%02X ", data[i]);
    }
    if (len > max_len) {
        fprintf(stderr, "...");
    }
    fprintf(stderr, "\n");
    
    /* Also show as ASCII where printable */
    fprintf(stderr, "%s ASCII: ", prefix);
    for (size_t i = 0; i < display; i++) {
        char c = (char)data[i];
        if (c >= 32 && c < 127) {
            fprintf(stderr, " %c ", c);
        } else {
            fprintf(stderr, " . ");
        }
    }
    fprintf(stderr, "\n");
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
    
    fprintf(stderr, "[TCP] Client %u connected from %s:%u (fd=%d)\n", 
            client_id, addr_str, port, fd);
    
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
    
    /* Debug: Print expected binary message sizes */
    fprintf(stderr, "[TCP] Client %u: Expected binary sizes: NewOrder=%zu Cancel=%zu Flush=%zu\n",
            client_id, sizeof(binary_new_order_t), sizeof(binary_cancel_t), sizeof(binary_flush_t));
    fprintf(stderr, "[TCP] Client %u: BINARY_MAGIC=0x%02X\n", client_id, BINARY_MAGIC);
    
    while (!atomic_load(&g_shutdown)) {
        /* Receive data */
        ssize_t n = recv(fd, recv_buffer + buffer_used, 
                         TCP_RECV_BUFFER_SIZE - buffer_used, 0);
        
        if (n <= 0) {
            if (n == 0) {
                fprintf(stderr, "[TCP] Client %u: Connection closed by peer\n", client_id);
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout - this is normal, just continue */
                continue;
            } else {
                fprintf(stderr, "[TCP] Client %u: recv error: %s (errno=%d)\n", 
                        client_id, strerror(errno), errno);
            }
            break;
        }
        
        buffer_used += n;
        
        fprintf(stderr, "[TCP] Client %u: Received %zd bytes (buffer now: %zu)\n", 
                client_id, n, buffer_used);
        hex_dump("[TCP] Data", recv_buffer, buffer_used, 64);
        
        /* Detect protocol on first data */
        if (protocol == CLIENT_PROTOCOL_UNKNOWN && buffer_used > 0) {
            fprintf(stderr, "[TCP] Client %u: Detecting protocol, first byte=0x%02X '%c'\n"

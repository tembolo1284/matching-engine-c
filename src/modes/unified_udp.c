#include "unified_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "protocol/csv/message_parser.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/binary/binary_protocol.h"

/* ============================================================================
 * UDP Receiver Thread
 * ============================================================================ */

void* unified_udp_receiver_thread(void* arg) {
    unified_server_t* server = (unified_server_t*)arg;
    
    fprintf(stderr, "[UDP] Receiver started on port %u\n", server->config.udp_port);
    
    uint8_t* recv_buffer = malloc(MAX_UDP_PACKET_SIZE);
    if (!recv_buffer) {
        fprintf(stderr, "[UDP] Failed to allocate receive buffer\n");
        return NULL;
    }
    
    message_parser_t csv_parser;
    message_parser_init(&csv_parser);
    
    binary_message_parser_t bin_parser;
    binary_message_parser_init(&bin_parser);
    
    while (!atomic_load(&g_shutdown)) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        ssize_t n = recvfrom(server->udp_fd, recv_buffer, MAX_UDP_PACKET_SIZE, 0,
                             (struct sockaddr*)&client_addr, &addr_len);
        
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            if (!atomic_load(&g_shutdown)) {
                fprintf(stderr, "[UDP] Recv error: %s\n", strerror(errno));
            }
            continue;
        }
        
        /* Get or create client entry */
        udp_client_addr_t compact_addr;
        compact_addr.addr = client_addr.sin_addr.s_addr;
        compact_addr.port = client_addr.sin_port;
        
        uint32_t client_id = client_registry_get_or_add_udp(server->registry, compact_addr);
        if (client_id == 0) {
            continue;  /* Registry full */
        }
        
        /* Detect and set protocol */
        client_protocol_t protocol = client_registry_get_protocol(server->registry, client_id);
        if (protocol == CLIENT_PROTOCOL_UNKNOWN) {
            protocol = unified_detect_protocol(recv_buffer, n);
            if (protocol != CLIENT_PROTOCOL_UNKNOWN) {
                client_registry_set_protocol(server->registry, client_id, protocol);
            }
        }
        
        /* Process messages */
        size_t offset = 0;
        
        if (protocol == CLIENT_PROTOCOL_BINARY) {
            /* Binary: may contain multiple messages */
            while (offset < (size_t)n) {
                size_t remaining = n - offset;
                const uint8_t* msg_start = recv_buffer + offset;
                
                if (remaining < 2) break;
                
                if (msg_start[0] != BINARY_MAGIC) {
                    offset++;
                    continue;
                }
                
                size_t msg_size = 0;
                uint8_t msg_type = msg_start[1];
                switch (msg_type) {
                    case 'N': msg_size = sizeof(binary_new_order_t); break;
                    case 'C': msg_size = sizeof(binary_cancel_t); break;
                    case 'F': msg_size = sizeof(binary_flush_t); break;
                    default:
                        offset++;
                        continue;
                }
                
                if (remaining < msg_size) break;
                
                input_msg_t input;
                if (binary_message_parser_parse(&bin_parser, msg_start, msg_size, &input)) {
                    unified_route_input(server, &input, client_id, &compact_addr);
                    atomic_fetch_add(&server->udp_messages_received, 1);
                    client_registry_inc_received(server->registry, client_id);
                }
                
                offset += msg_size;
            }
        } else {
            /* CSV: newline-delimited, may have multiple lines */
            recv_buffer[n] = '\0';  /* Ensure null-terminated */
            
            char* line = (char*)recv_buffer;
            char* next;
            
            while (line && *line) {
                next = strchr(line, '\n');
                if (next) {
                    *next = '\0';
                    next++;
                }
                
                /* Skip empty lines */
                if (*line == '\0' || *line == '\r') {
                    line = next;
                    continue;
                }
                
                input_msg_t input;
                if (message_parser_parse(&csv_parser, line, &input)) {
                    unified_route_input(server, &input, client_id, &compact_addr);
                    atomic_fetch_add(&server->udp_messages_received, 1);
                    client_registry_inc_received(server->registry, client_id);
                }
                
                line = next;
            }
        }
        
        client_registry_touch(server->registry, client_id);
    }
    
    free(recv_buffer);
    fprintf(stderr, "[UDP] Receiver stopped\n");
    return NULL;
}

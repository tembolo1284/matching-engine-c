#include "modes/unified_mode.h"
#include "unified_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "threading/processor.h"
#include "protocol/binary/binary_protocol.h"

/* ============================================================================
 * User-Client Mapping Implementation
 * ============================================================================ */

void user_client_map_init(user_client_map_t* map) {
    memset(map->mappings, 0, sizeof(map->mappings));
    pthread_rwlock_init(&map->lock, NULL);
}

void user_client_map_destroy(user_client_map_t* map) {
    pthread_rwlock_destroy(&map->lock);
}

void user_client_map_set(user_client_map_t* map, uint32_t user_id, uint32_t client_id) {
    if (user_id == 0) return;
    
    uint32_t slot = user_id % MAX_USER_ID_MAPPINGS;
    
    pthread_rwlock_wrlock(&map->lock);
    map->mappings[slot].user_id = user_id;
    map->mappings[slot].client_id = client_id;
    map->mappings[slot].active = true;
    pthread_rwlock_unlock(&map->lock);
}

uint32_t user_client_map_get(user_client_map_t* map, uint32_t user_id) {
    if (user_id == 0) return 0;
    
    uint32_t slot = user_id % MAX_USER_ID_MAPPINGS;
    uint32_t result = 0;
    
    pthread_rwlock_rdlock(&map->lock);
    if (map->mappings[slot].active && map->mappings[slot].user_id == user_id) {
        result = map->mappings[slot].client_id;
    }
    pthread_rwlock_unlock(&map->lock);
    
    return result;
}

/* ============================================================================
 * Helper: 64-byte aligned allocation
 * ============================================================================ */

static inline void* aligned_alloc_64(size_t size) {
    void* ptr = NULL;
    if (posix_memalign(&ptr, 64, size) != 0) {
        return NULL;
    }
    return ptr;
}

/* ============================================================================
 * Shared Helper Implementations
 * ============================================================================ */

client_protocol_t unified_detect_protocol(const uint8_t* data, size_t len) {
    if (len == 0) return CLIENT_PROTOCOL_UNKNOWN;
    
    /* Binary: starts with magic byte 0x4D ('M') */
    if (data[0] == BINARY_MAGIC) {
        return CLIENT_PROTOCOL_BINARY;
    }
    
    /* CSV: starts with command letter N, C, F */
    if (data[0] == 'N' || data[0] == 'C' || data[0] == 'F') {
        return CLIENT_PROTOCOL_CSV;
    }
    
    return CLIENT_PROTOCOL_UNKNOWN;
}

void unified_route_input(unified_server_t* server,
                         const input_msg_t* input,
                         uint32_t client_id,
                         const udp_client_addr_t* udp_addr) {
    /* Record user → client mapping for trades */
    if (input->type == INPUT_MSG_NEW_ORDER) {
        user_client_map_set(server->user_map, input->data.new_order.user_id, client_id);
    }
    
    /* Create envelope */
    input_msg_envelope_t envelope;
    envelope.msg = *input;
    envelope.client_id = client_id;
    envelope.sequence = 0;
    
    if (udp_addr) {
        envelope.client_addr = *udp_addr;
    } else {
        memset(&envelope.client_addr, 0, sizeof(envelope.client_addr));
    }
    
    /* Get symbol for routing */
    const char* symbol = NULL;
    if (input->type == INPUT_MSG_NEW_ORDER) {
        symbol = input->data.new_order.symbol;
    } else if (input->type == INPUT_MSG_CANCEL) {
        symbol = input->data.cancel.symbol;
    }
    
    /* Route to appropriate processor */
    if (server->config.single_processor || get_processor_for_symbol(symbol) == 0) {
        input_envelope_queue_enqueue(server->input_queue_0, &envelope);
    } else {
        input_envelope_queue_enqueue(server->input_queue_1, &envelope);
    }
}

/* ============================================================================
 * Print Statistics
 * ============================================================================ */

static void print_server_stats(unified_server_t* server) {
    fprintf(stderr, "\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "      Unified Server Statistics\n");
    fprintf(stderr, "========================================\n");
    
    fprintf(stderr, "\n--- Message Counts ---\n");
    fprintf(stderr, "TCP messages received:    %llu\n",
            (unsigned long long)atomic_load(&server->tcp_messages_received));
    fprintf(stderr, "UDP messages received:    %llu\n",
            (unsigned long long)atomic_load(&server->udp_messages_received));
    fprintf(stderr, "Messages routed:          %llu\n",
            (unsigned long long)atomic_load(&server->messages_routed));
    fprintf(stderr, "Multicast messages:       %llu\n",
            (unsigned long long)atomic_load(&server->multicast_messages));
    fprintf(stderr, "TOB broadcasts:           %llu\n",
            (unsigned long long)atomic_load(&server->tob_broadcasts));
    
    client_registry_print_stats(server->registry);
}

/* ============================================================================
 * Server Initialization
 * ============================================================================ */

static bool init_memory_and_engines(unified_server_t* server) {
    /* Allocate memory pools */
    server->pools_0 = aligned_alloc_64(sizeof(memory_pools_t));
    if (!server->pools_0) {
        fprintf(stderr, "[Unified] Failed to allocate memory pool 0\n");
        return false;
    }
    memory_pools_init(server->pools_0);
    
    if (!server->config.single_processor) {
        server->pools_1 = aligned_alloc_64(sizeof(memory_pools_t));
        if (!server->pools_1) {
            fprintf(stderr, "[Unified] Failed to allocate memory pool 1\n");
            return false;
        }
        memory_pools_init(server->pools_1);
    }
    
    /* Allocate matching engines */
    server->engine_0 = malloc(sizeof(matching_engine_t));
    if (!server->engine_0) {
        fprintf(stderr, "[Unified] Failed to allocate engine 0\n");
        return false;
    }
    matching_engine_init(server->engine_0, server->pools_0);
    
    if (!server->config.single_processor) {
        server->engine_1 = malloc(sizeof(matching_engine_t));
        if (!server->engine_1) {
            fprintf(stderr, "[Unified] Failed to allocate engine 1\n");
            return false;
        }
        matching_engine_init(server->engine_1, server->pools_1);
    }
    
    return true;
}

static bool init_queues(unified_server_t* server) {
    server->input_queue_0 = malloc(sizeof(input_envelope_queue_t));
    server->output_queue_0 = malloc(sizeof(output_envelope_queue_t));
    if (!server->input_queue_0 || !server->output_queue_0) {
        fprintf(stderr, "[Unified] Failed to allocate queues\n");
        return false;
    }
    input_envelope_queue_init(server->input_queue_0);
    output_envelope_queue_init(server->output_queue_0);
    
    if (!server->config.single_processor) {
        server->input_queue_1 = malloc(sizeof(input_envelope_queue_t));
        server->output_queue_1 = malloc(sizeof(output_envelope_queue_t));
        if (!server->input_queue_1 || !server->output_queue_1) {
            fprintf(stderr, "[Unified] Failed to allocate queues for processor 1\n");
            return false;
        }
        input_envelope_queue_init(server->input_queue_1);
        output_envelope_queue_init(server->output_queue_1);
    }
    
    return true;
}

static bool init_client_tracking(unified_server_t* server) {
    server->registry = malloc(sizeof(client_registry_t));
    if (!server->registry) {
        fprintf(stderr, "[Unified] Failed to allocate client registry\n");
        return false;
    }
    client_registry_init(server->registry);
    
    server->user_map = malloc(sizeof(user_client_map_t));
    if (!server->user_map) {
        fprintf(stderr, "[Unified] Failed to allocate user map\n");
        return false;
    }
    user_client_map_init(server->user_map);
    
    return true;
}

static bool init_tcp_socket(unified_server_t* server) {
    if (server->config.disable_tcp) return true;
    
    server->tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->tcp_listen_fd < 0) {
        fprintf(stderr, "[Unified] Failed to create TCP socket: %s\n", strerror(errno));
        return false;
    }
    
    int opt = 1;
    setsockopt(server->tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in tcp_addr;
    memset(&tcp_addr, 0, sizeof(tcp_addr));
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_addr.s_addr = INADDR_ANY;
    tcp_addr.sin_port = htons(server->config.tcp_port);
    
    if (bind(server->tcp_listen_fd, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) < 0) {
        fprintf(stderr, "[Unified] Failed to bind TCP socket: %s\n", strerror(errno));
        return false;
    }
    
    if (listen(server->tcp_listen_fd, TCP_LISTEN_BACKLOG) < 0) {
        fprintf(stderr, "[Unified] Failed to listen on TCP socket: %s\n", strerror(errno));
        return false;
    }
    
    /* Set timeout for accept */
    struct timeval tv = {1, 0};
    setsockopt(server->tcp_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    fprintf(stderr, "✓ TCP listener bound to port %u\n", server->config.tcp_port);
    return true;
}

static bool init_udp_socket(unified_server_t* server) {
    if (server->config.disable_udp) return true;
    
    server->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->udp_fd < 0) {
        fprintf(stderr, "[Unified] Failed to create UDP socket: %s\n", strerror(errno));
        return false;
    }
    
    int opt = 1;
    setsockopt(server->udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /* Large receive buffer */
    int rcvbuf = UDP_RECV_BUFFER_SIZE;
    setsockopt(server->udp_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(server->config.udp_port);
    
    if (bind(server->udp_fd, (struct sockaddr*)&udp_addr, sizeof(udp_addr)) < 0) {
        fprintf(stderr, "[Unified] Failed to bind UDP socket: %s\n", strerror(errno));
        return false;
    }
    
    /* Set timeout for recv */
    struct timeval tv = {1, 0};
    setsockopt(server->udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    fprintf(stderr, "✓ UDP receiver bound to port %u\n", server->config.udp_port);
    return true;
}

static bool init_multicast_socket(unified_server_t* server) {
    if (server->config.disable_multicast) return true;
    
    server->multicast_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->multicast_fd < 0) {
        fprintf(stderr, "⚠ Multicast disabled (socket failed)\n");
        return true;  /* Non-fatal */
    }
    
    /* Set multicast TTL */
    int ttl = 1;
    setsockopt(server->multicast_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    
    /* Disable loopback */
    int loop = 1;
    setsockopt(server->multicast_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    
    /* Setup multicast address */
    const char* group = server->config.multicast_group ? 
                        server->config.multicast_group : UNIFIED_MULTICAST_GROUP;
    
    memset(&server->multicast_addr, 0, sizeof(server->multicast_addr));
    server->multicast_addr.sin_family = AF_INET;
    server->multicast_addr.sin_port = htons(server->config.multicast_port);
    
    if (inet_pton(AF_INET, group, &server->multicast_addr.sin_addr) <= 0) {
        fprintf(stderr, "⚠ Multicast disabled (invalid group: %s)\n", group);
        close(server->multicast_fd);
        server->multicast_fd = -1;
        return true;  /* Non-fatal */
    }
    
    fprintf(stderr, "✓ Multicast publisher initialized on %s:%u\n", 
            group, server->config.multicast_port);
    return true;
}

/* ============================================================================
 * Server Cleanup
 * ============================================================================ */

static void cleanup_server(unified_server_t* server) {
    /* Close sockets (may already be closed during shutdown) */
    if (server->tcp_listen_fd >= 0) close(server->tcp_listen_fd);
    if (server->udp_fd >= 0) close(server->udp_fd);
    if (server->multicast_fd >= 0) close(server->multicast_fd);
    
    /* Cleanup user map */
    if (server->user_map) {
        user_client_map_destroy(server->user_map);
        free(server->user_map);
    }
    
    /* Cleanup registry */
    if (server->registry) {
        client_registry_destroy(server->registry);
        free(server->registry);
    }
    
    /* Cleanup queues */
    if (server->input_queue_0) {
        input_envelope_queue_destroy(server->input_queue_0);
        free(server->input_queue_0);
    }
    if (server->input_queue_1) {
        input_envelope_queue_destroy(server->input_queue_1);
        free(server->input_queue_1);
    }
    if (server->output_queue_0) {
        output_envelope_queue_destroy(server->output_queue_0);
        free(server->output_queue_0);
    }
    if (server->output_queue_1) {
        output_envelope_queue_destroy(server->output_queue_1);
        free(server->output_queue_1);
    }
    
    /* Cleanup engines */
    if (server->engine_0) {
        matching_engine_destroy(server->engine_0);
        free(server->engine_0);
    }
    if (server->engine_1) {
        matching_engine_destroy(server->engine_1);
        free(server->engine_1);
    }
    
    /* Cleanup pools */
    if (server->pools_0) free(server->pools_0);
    if (server->pools_1) free(server->pools_1);
    
    free(server);
}

/* ============================================================================
 * Main Server Function
 * ============================================================================ */

int run_unified_server(const unified_config_t* config) {
    fprintf(stderr, "\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "    Matching Engine - Unified Server\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "TCP Port:       %u\n", config->tcp_port);
    fprintf(stderr, "UDP Port:       %u\n", config->udp_port);
    fprintf(stderr, "Multicast:      %s:%u\n", 
            config->multicast_group ? config->multicast_group : UNIFIED_MULTICAST_GROUP,
            config->multicast_port);
    fprintf(stderr, "Processors:     %s\n", config->single_processor ? "Single" : "Dual (A-M/N-Z)");
    fprintf(stderr, "Quiet mode:     %s\n", config->quiet_mode ? "Yes" : "No");
    fprintf(stderr, "========================================\n\n");
    
    int ret = 1;
    
    /* Allocate server context */
    unified_server_t* server = calloc(1, sizeof(unified_server_t));
    if (!server) {
        fprintf(stderr, "[Unified] Failed to allocate server context\n");
        return 1;
    }
    
    server->config = *config;
    server->tcp_listen_fd = -1;
    server->udp_fd = -1;
    server->multicast_fd = -1;
    
    /* Initialize formatters */
    binary_message_formatter_init(&server->bin_formatter);
    memset(&server->csv_formatter, 0, sizeof(server->csv_formatter));
    
    /* Initialize components */
    if (!init_memory_and_engines(server)) goto cleanup;
    if (!init_queues(server)) goto cleanup;
    if (!init_client_tracking(server)) goto cleanup;
    if (!init_tcp_socket(server)) goto cleanup;
    if (!init_udp_socket(server)) goto cleanup;
    if (!init_multicast_socket(server)) goto cleanup;
    
    /* Create processor contexts */
    processor_t processor_0;
    processor_t processor_1;
    
    processor_config_t proc_config_0 = {
        .processor_id = 0,
        .tcp_mode = false
    };
    
    if (!processor_init(&processor_0, &proc_config_0, server->engine_0,
                        server->input_queue_0, server->output_queue_0, &g_shutdown)) {
        fprintf(stderr, "[Unified] Failed to init processor 0\n");
        goto cleanup;
    }
    
    if (!config->single_processor) {
        processor_config_t proc_config_1 = {
            .processor_id = 1,
            .tcp_mode = false
        };
        
        if (!processor_init(&processor_1, &proc_config_1, server->engine_1,
                            server->input_queue_1, server->output_queue_1, &g_shutdown)) {
            fprintf(stderr, "[Unified] Failed to init processor 1\n");
            goto cleanup;
        }
    }
    
    /* Start threads */
    pthread_t tcp_tid, udp_tid, proc0_tid, proc1_tid, router_tid;
    bool tcp_started = false, udp_started = false;
    bool proc0_started = false, proc1_started = false, router_started = false;
    
    /* TCP listener */
    if (!config->disable_tcp) {
        if (pthread_create(&tcp_tid, NULL, unified_tcp_listener_thread, server) != 0) {
            fprintf(stderr, "[Unified] Failed to start TCP listener\n");
            goto cleanup_threads;
        }
        tcp_started = true;
    }
    
    /* UDP receiver */
    if (!config->disable_udp) {
        if (pthread_create(&udp_tid, NULL, unified_udp_receiver_thread, server) != 0) {
            fprintf(stderr, "[Unified] Failed to start UDP receiver\n");
            goto cleanup_threads;
        }
        udp_started = true;
    }
    
    /* Processor 0 */
    if (pthread_create(&proc0_tid, NULL, processor_thread, &processor_0) != 0) {
        fprintf(stderr, "[Unified] Failed to start processor 0\n");
        goto cleanup_threads;
    }
    proc0_started = true;
    
    /* Processor 1 */
    if (!config->single_processor) {
        if (pthread_create(&proc1_tid, NULL, processor_thread, &processor_1) != 0) {
            fprintf(stderr, "[Unified] Failed to start processor 1\n");
            goto cleanup_threads;
        }
        proc1_started = true;
    }
    
    /* Output router */
    if (pthread_create(&router_tid, NULL, unified_output_router_thread, server) != 0) {
        fprintf(stderr, "[Unified] Failed to start output router\n");
        goto cleanup_threads;
    }
    router_started = true;
    
    fprintf(stderr, "\n");
    fprintf(stderr, "✓ All threads started successfully\n");
    fprintf(stderr, "  - TCP Listener%s\n", config->disable_tcp ? " (disabled)" : "");
    fprintf(stderr, "  - UDP Receiver%s\n", config->disable_udp ? " (disabled)" : "");
    fprintf(stderr, "  - Processor 0 (A-M)\n");
    if (!config->single_processor) {
        fprintf(stderr, "  - Processor 1 (N-Z)\n");
    }
    fprintf(stderr, "  - Output Router\n");
    fprintf(stderr, "  - Multicast Publisher%s\n", 
            config->disable_multicast || server->multicast_fd < 0 ? " (disabled)" : "");
    fprintf(stderr, "\n");
    fprintf(stderr, "✓ Server ready - Press Ctrl+C to stop\n\n");
    
    /* Wait for shutdown */
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }
    
    fprintf(stderr, "\n[Unified] Shutdown signal received\n");
    ret = 0;
    
cleanup_threads:
    atomic_store(&g_shutdown, true);
    
    /* Close sockets FIRST to unblock accept()/recvfrom() in listener threads */
    if (server->tcp_listen_fd >= 0) {
        shutdown(server->tcp_listen_fd, SHUT_RDWR);
        close(server->tcp_listen_fd);
        server->tcp_listen_fd = -1;
    }
    if (server->udp_fd >= 0) {
        close(server->udp_fd);
        server->udp_fd = -1;
    }
    
    /* Now threads can exit and be joined */
    if (router_started) pthread_join(router_tid, NULL);
    if (proc1_started) pthread_join(proc1_tid, NULL);
    if (proc0_started) pthread_join(proc0_tid, NULL);
    if (udp_started) pthread_join(udp_tid, NULL);
    if (tcp_started) pthread_join(tcp_tid, NULL);
    
    /* Print statistics */
    print_server_stats(server);
    
    if (proc0_started) {
        fprintf(stderr, "\n--- Processor 0 (A-M) ---\n");
        processor_print_stats(&processor_0);
    }
    if (proc1_started) {
        fprintf(stderr, "\n--- Processor 1 (N-Z) ---\n");
        processor_print_stats(&processor_1);
    }
    
cleanup:
    cleanup_server(server);
    
    fprintf(stderr, "\n=== Unified Server Stopped ===\n");
    return ret;
}

#include "threading/output_router.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_formatter.h"

#define BATCH_SIZE 32
#define SLEEP_TIME_NS 1000  /* 1 microsecond */

static const struct timespec ts = {
    .tv_sec = 0,
    .tv_nsec = SLEEP_TIME_NS
};

/* ============================================================================
 * Multicast Socket Setup (integrated into router)
 * ============================================================================ */

static bool setup_multicast_socket(output_router_context_t* ctx) {
    ctx->mcast_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->mcast_sockfd < 0) {
        fprintf(stderr, "[Output Router] ERROR: Failed to create multicast socket: %s\n",
                strerror(errno));
        return false;
    }

    /* Set multicast TTL */
    unsigned char ttl = ctx->mcast_config.ttl;
    if (setsockopt(ctx->mcast_sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        fprintf(stderr, "[Output Router] WARNING: Failed to set multicast TTL: %s\n",
                strerror(errno));
    }

    /* Disable loopback - don't receive our own packets */
    unsigned char loop = 0;
    if (setsockopt(ctx->mcast_sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        fprintf(stderr, "[Output Router] WARNING: Failed to disable multicast loopback: %s\n",
                strerror(errno));
    }

    /* Configure destination address */
    memset(&ctx->mcast_addr, 0, sizeof(ctx->mcast_addr));
    ctx->mcast_addr.sin_family = AF_INET;
    ctx->mcast_addr.sin_port = htons(ctx->mcast_config.port);
    
    if (inet_pton(AF_INET, ctx->mcast_config.multicast_group, &ctx->mcast_addr.sin_addr) <= 0) {
        fprintf(stderr, "[Output Router] ERROR: Invalid multicast group: %s\n",
                ctx->mcast_config.multicast_group);
        close(ctx->mcast_sockfd);
        ctx->mcast_sockfd = -1;
        return false;
    }

    fprintf(stderr, "[Output Router] Multicast enabled: %s:%u (TTL=%u)\n",
            ctx->mcast_config.multicast_group, ctx->mcast_config.port, ttl);

    return true;
}

static void cleanup_multicast_socket(output_router_context_t* ctx) {
    if (ctx->mcast_sockfd >= 0) {
        close(ctx->mcast_sockfd);
        ctx->mcast_sockfd = -1;
    }
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool output_router_init(output_router_context_t* ctx,
                        const output_router_config_t* config,
                        tcp_client_registry_t* client_registry,
                        output_envelope_queue_t* input_queue,
                        atomic_bool* shutdown_flag) {
    memset(ctx, 0, sizeof(*ctx));
    
    ctx->config = *config;
    ctx->client_registry = client_registry;
    ctx->input_queues[0] = input_queue;
    ctx->input_queues[1] = NULL;
    ctx->num_input_queues = 1;
    ctx->input_queue = input_queue;
    ctx->shutdown_flag = shutdown_flag;
    ctx->mcast_sockfd = -1;
    ctx->mcast_enabled = false;
    
    return true;
}

bool output_router_init_dual(output_router_context_t* ctx,
                             const output_router_config_t* config,
                             tcp_client_registry_t* client_registry,
                             output_envelope_queue_t* input_queue_0,
                             output_envelope_queue_t* input_queue_1,
                             atomic_bool* shutdown_flag) {
    memset(ctx, 0, sizeof(*ctx));
    
    ctx->config = *config;
    ctx->client_registry = client_registry;
    ctx->input_queues[0] = input_queue_0;
    ctx->input_queues[1] = input_queue_1;
    ctx->num_input_queues = 2;
    ctx->input_queue = input_queue_0;
    ctx->shutdown_flag = shutdown_flag;
    ctx->mcast_sockfd = -1;
    ctx->mcast_enabled = false;
    
    return true;
}

bool output_router_enable_multicast(output_router_context_t* ctx,
                                    const char* multicast_group,
                                    uint16_t port,
                                    uint8_t ttl,
                                    bool use_binary) {
    /* Store config */
    snprintf(ctx->mcast_config.multicast_group, 
             sizeof(ctx->mcast_config.multicast_group),
             "%s", multicast_group);
    ctx->mcast_config.port = port;
    ctx->mcast_config.ttl = ttl;
    ctx->mcast_config.use_binary_output = use_binary;

    /* Setup socket */
    if (!setup_multicast_socket(ctx)) {
        return false;
    }

    ctx->mcast_enabled = true;
    return true;
}

void output_router_cleanup(output_router_context_t* ctx) {
    cleanup_multicast_socket(ctx);
}

/* ============================================================================
 * Message Routing
 * ============================================================================ */

/**
 * Broadcast message to multicast group
 */
static bool broadcast_to_multicast(output_router_context_t* ctx,
                                   const output_msg_t* msg) {
    if (!ctx->mcast_enabled || ctx->mcast_sockfd < 0) {
        return false;
    }

    size_t msg_len = 0;
    const void* payload = NULL;

    if (ctx->mcast_config.use_binary_output) {
        binary_message_formatter_t formatter;
        memset(&formatter, 0, sizeof(formatter));
        payload = binary_message_formatter_format(&formatter, msg, &msg_len);
        if (!payload || msg_len == 0) {
            return false;
        }
    } else {
        message_formatter_t formatter;
        memset(&formatter, 0, sizeof(formatter));
        const char* text = message_formatter_format(&formatter, msg);
        if (!text) {
            return false;
        }
        payload = text;
        msg_len = strlen(text);
    }

    ssize_t sent = sendto(ctx->mcast_sockfd, payload, msg_len, 0,
                          (struct sockaddr*)&ctx->mcast_addr,
                          sizeof(ctx->mcast_addr));

    if (sent < 0) {
        ctx->mcast_errors++;
        return false;
    }

    ctx->mcast_messages++;
    return true;
}

/**
 * Route a single message to the appropriate client AND broadcast to multicast
 */
static bool route_single_message(output_router_context_t* ctx,
                                  output_msg_envelope_t* envelope) {
    bool routed_to_client = false;

    if (ctx->config.tcp_mode) {
        /* TCP mode - route to specific client */
        tcp_client_t* client = tcp_client_get(ctx->client_registry, 
                                               envelope->client_id);
        
        if (client) {
            if (tcp_client_enqueue_output(client, &envelope->msg)) {
                routed_to_client = true;
            } else {
                fprintf(stderr, "[Output Router] Client %u queue full, dropping message\n",
                        envelope->client_id);
            }
        }
        /* Client disconnected - message still goes to multicast */
    } else {
        /* UDP mode - just count as routed for now */
        routed_to_client = true;
    }

    /* ALWAYS broadcast to multicast (if enabled) regardless of TCP routing */
    if (ctx->mcast_enabled) {
        broadcast_to_multicast(ctx, &envelope->msg);
    }

    return routed_to_client;
}

/* ============================================================================
 * Router Thread
 * ============================================================================ */

void* output_router_thread(void* arg) {
    output_router_context_t* ctx = (output_router_context_t*)arg;
    
    fprintf(stderr, "[Output Router] Starting (mode: %s, queues: %d, multicast: %s)\n",
            ctx->config.tcp_mode ? "TCP" : "UDP",
            ctx->num_input_queues,
            ctx->mcast_enabled ? "enabled" : "disabled");
    
    output_msg_envelope_t batch[BATCH_SIZE];
    
    while (!atomic_load(ctx->shutdown_flag)) {
        size_t total_processed = 0;
        
        /* Round-robin across all input queues for fairness */
        for (int q = 0; q < ctx->num_input_queues; q++) {
            output_envelope_queue_t* queue = ctx->input_queues[q];
            if (!queue) continue;
            
            /* Dequeue a batch */
            size_t count = 0;
            for (size_t i = 0; i < BATCH_SIZE; i++) {
                if (output_envelope_queue_dequeue(queue, &batch[count])) {
                    count++;
                } else {
                    break;
                }
            }
            
            /* Route each message */
            for (size_t i = 0; i < count; i++) {
                output_msg_envelope_t* envelope = &batch[i];
                
                if (route_single_message(ctx, envelope)) {
                    ctx->messages_routed++;
                } else {
                    ctx->messages_dropped++;
                }
                
                ctx->messages_from_processor[q]++;
            }
            
            total_processed += count;
        }
        
        if (total_processed == 0) {
            nanosleep(&ts, NULL);
        }
    }
    
    /* Drain remaining messages */
    fprintf(stderr, "[Output Router] Draining remaining messages...\n");
    
    bool has_messages = true;
    int drain_iterations = 0;
    const int MAX_DRAIN_ITERATIONS = 100;
    
    while (has_messages && drain_iterations < MAX_DRAIN_ITERATIONS) {
        has_messages = false;
        drain_iterations++;
        
        for (int q = 0; q < ctx->num_input_queues; q++) {
            output_envelope_queue_t* queue = ctx->input_queues[q];
            if (!queue) continue;
            
            output_msg_envelope_t envelope;
            while (output_envelope_queue_dequeue(queue, &envelope)) {
                has_messages = true;
                
                if (route_single_message(ctx, &envelope)) {
                    ctx->messages_routed++;
                } else {
                    ctx->messages_dropped++;
                }
                
                ctx->messages_from_processor[q]++;
            }
        }
    }
    
    fprintf(stderr, "[Output Router] Shutting down\n");
    output_router_print_stats(ctx);
    
    return NULL;
}

void output_router_print_stats(const output_router_context_t* ctx) {
    fprintf(stderr, "\n=== Output Router Statistics ===\n");
    fprintf(stderr, "Messages routed:       %llu\n", (unsigned long long)ctx->messages_routed);
    fprintf(stderr, "Messages dropped:      %llu\n", (unsigned long long)ctx->messages_dropped);
    
    if (ctx->num_input_queues == 2) {
        fprintf(stderr, "From Processor 0 (A-M): %llu\n", 
                (unsigned long long)ctx->messages_from_processor[0]);
        fprintf(stderr, "From Processor 1 (N-Z): %llu\n", 
                (unsigned long long)ctx->messages_from_processor[1]);
    }

    if (ctx->mcast_enabled) {
        fprintf(stderr, "Multicast messages:    %llu\n", (unsigned long long)ctx->mcast_messages);
        fprintf(stderr, "Multicast errors:      %llu\n", (unsigned long long)ctx->mcast_errors);
    }
}

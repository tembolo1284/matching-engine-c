#include "threading/output_router.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BATCH_SIZE 32
#define SLEEP_TIME_US 100

static const struct timespec ts = {
    .tv_sec = 0,
    .tv_nsec = 1000
};

bool output_router_init(output_router_context_t* ctx,
                        const output_router_config_t* config,
                        tcp_client_registry_t* client_registry,
                        output_envelope_queue_t* input_queue,
                        atomic_bool* shutdown_flag) {
    memset(ctx, 0, sizeof(*ctx));
    
    ctx->config = *config;
    ctx->client_registry = client_registry;
    ctx->input_queue = input_queue;
    ctx->shutdown_flag = shutdown_flag;
    
    return true;
}

void output_router_cleanup(output_router_context_t* ctx) {
    // Nothing to cleanup currently
    (void)ctx;
}

void* output_router_thread(void* arg) {
    output_router_context_t* ctx = (output_router_context_t*)arg;
    
    fprintf(stderr, "[Output Router] Starting (mode: %s)\n",
            ctx->config.tcp_mode ? "TCP" : "UDP");
    
    output_msg_envelope_t batch[BATCH_SIZE];
    
    while (!atomic_load(ctx->shutdown_flag)) {
        // Dequeue a batch of output envelopes
        size_t count = 0;
        for (size_t i = 0; i < BATCH_SIZE; i++) {
            if (output_envelope_queue_dequeue(ctx->input_queue, &batch[count])) {
                count++;
            } else {
                break;
            }
        }
        
        if (count == 0) {
            // No messages - sleep briefly
            nanosleep(&ts, NULL);
            continue;
        }
        
        // Route each message
        for (size_t i = 0; i < count; i++) {
            output_msg_envelope_t* envelope = &batch[i];
            
            if (ctx->config.tcp_mode) {
                // TCP mode - route to specific client
                tcp_client_t* client = tcp_client_get(ctx->client_registry, 
                                                       envelope->client_id);
                
                if (client) {
                    // Enqueue to client's output queue
                    if (tcp_client_enqueue_output(client, &envelope->msg)) {
                        ctx->messages_routed++;
                    } else {
                        // Client's queue is full
                        fprintf(stderr, "[Output Router] Client %u queue full, dropping message\n",
                                envelope->client_id);
                        ctx->messages_dropped++;
                    }
                } else {
                    // Client disconnected - drop message
                    ctx->messages_dropped++;
                }
            } else {
                // UDP mode - would write to stdout queue (not implemented yet)
                // For now, just count
                ctx->messages_routed++;
            }
        }
    }
    
    fprintf(stderr, "[Output Router] Shutting down\n");
    output_router_print_stats(ctx);
    
    return NULL;
}

void output_router_print_stats(const output_router_context_t* ctx) {
    fprintf(stderr, "\n=== Output Router Statistics ===\n");
    fprintf(stderr, "Messages routed:       %lu\n", ctx->messages_routed);
    fprintf(stderr, "Messages dropped:      %lu\n", ctx->messages_dropped);
}

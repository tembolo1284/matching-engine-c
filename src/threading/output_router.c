#include "threading/output_router.h"

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BATCH_SIZE 32
#define SLEEP_TIME_NS 1000  // 1 microsecond

static const struct timespec ts = {
    .tv_sec = 0,
    .tv_nsec = SLEEP_TIME_NS
};

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
    ctx->input_queue = input_queue;  // Backward compatibility
    ctx->shutdown_flag = shutdown_flag;
    
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
    ctx->input_queue = input_queue_0;  // Backward compatibility
    ctx->shutdown_flag = shutdown_flag;
    
    return true;
}

void output_router_cleanup(output_router_context_t* ctx) {
    // Nothing to cleanup currently
    (void)ctx;
}

/**
 * Route a single message to the appropriate client
 * Returns true if successfully routed, false if dropped
 */
static bool route_single_message(output_router_context_t* ctx,
                                  output_msg_envelope_t* envelope) {
    if (ctx->config.tcp_mode) {
        // TCP mode - route to specific client
        tcp_client_t* client = tcp_client_get(ctx->client_registry, 
                                               envelope->client_id);
        
        if (client) {
            // Enqueue to client's output queue
            if (tcp_client_enqueue_output(client, &envelope->msg)) {
                return true;
            } else {
                // Client's queue is full
                fprintf(stderr, "[Output Router] Client %u queue full, dropping message\n",
                        envelope->client_id);
                return false;
            }
        } else {
            // Client disconnected - drop message silently
            return false;
        }
    } else {
        // UDP mode - would write to stdout queue (not implemented yet)
        // For now, just count as routed
        return true;
    }
}

void* output_router_thread(void* arg) {
    output_router_context_t* ctx = (output_router_context_t*)arg;
    
    fprintf(stderr, "[Output Router] Starting (mode: %s, queues: %d)\n",
            ctx->config.tcp_mode ? "TCP" : "UDP",
            ctx->num_input_queues);
    
    output_msg_envelope_t batch[BATCH_SIZE];
    
    while (!atomic_load(ctx->shutdown_flag)) {
        size_t total_processed = 0;
        
        // Round-robin across all input queues
        // This ensures fairness - no processor can starve another
        for (int q = 0; q < ctx->num_input_queues; q++) {
            output_envelope_queue_t* queue = ctx->input_queues[q];
            if (!queue) continue;
            
            // Dequeue a batch from this queue
            size_t count = 0;
            for (size_t i = 0; i < BATCH_SIZE; i++) {
                if (output_envelope_queue_dequeue(queue, &batch[count])) {
                    count++;
                } else {
                    break;
                }
            }
            
            // Route each message
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
            // No messages from any queue - sleep briefly
            nanosleep(&ts, NULL);
        }
    }
    
    // Drain remaining messages before shutdown
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
}

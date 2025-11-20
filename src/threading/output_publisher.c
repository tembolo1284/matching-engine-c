#include "threading/output_publisher.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_formatter.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define BATCH_SIZE 32
#define SLEEP_TIME_US 100

// Define the queue (already declared in header)
DEFINE_LOCKFREE_QUEUE(output_msg_envelope_t, output_envelope_queue)

bool output_publisher_init(output_publisher_context_t* ctx,
                           const output_publisher_config_t* config,
                           output_envelope_queue_t* input_queue,
                           atomic_bool* shutdown_flag) {
    memset(ctx, 0, sizeof(*ctx));
    
    ctx->config = *config;
    ctx->input_queue = input_queue;
    ctx->shutdown_flag = shutdown_flag;
    
    return true;
}

void output_publisher_cleanup(output_publisher_context_t* ctx) {
    (void)ctx;
}

void* output_publisher_thread(void* arg) {
    output_publisher_context_t* ctx = (output_publisher_context_t*)arg;
    
    fprintf(stderr, "[Output Publisher] Starting (format: %s)\n",
            ctx->config.use_binary_output ? "Binary" : "CSV");
    
    output_msg_envelope_t batch[BATCH_SIZE];
    char output_buffer[4096];
    
    while (!atomic_load(ctx->shutdown_flag)) {
        // Dequeue batch of output envelopes
        size_t count = 0;
        for (size_t i = 0; i < BATCH_SIZE; i++) {
            if (output_envelope_queue_dequeue(ctx->input_queue, &batch[count])) {
                count++;
            } else {
                break;
            }
        }
        
        if (count == 0) {
            usleep(SLEEP_TIME_US);
            continue;
        }
        
        // Format and publish each message
        for (size_t i = 0; i < count; i++) {
            // Extract the actual message (ignore client_id in UDP mode)
            output_msg_t* msg = &batch[i].msg;
            
            size_t len;
            if (ctx->config.use_binary_output) {
                len = format_binary_message(msg, output_buffer, sizeof(output_buffer));
            } else {
                len = format_message(msg, output_buffer, sizeof(output_buffer));
            }
            
            if (len > 0) {
                // Write to stdout
                fwrite(output_buffer, 1, len, stdout);
                fflush(stdout);
                ctx->messages_published++;
            }
        }
    }
    
    fprintf(stderr, "[Output Publisher] Shutting down\n");
    output_publisher_print_stats(ctx);
    
    return NULL;
}

void output_publisher_print_stats(const output_publisher_context_t* ctx) {
    fprintf(stderr, "\n=== Output Publisher Statistics ===\n");
    fprintf(stderr, "Messages published:    %lu\n", ctx->messages_published);
}

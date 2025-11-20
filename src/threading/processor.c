#include "threading/processor.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define BATCH_SIZE 32
#define SLEEP_TIME_US 1

// Define queue implementations
DEFINE_LOCKFREE_QUEUE(input_msg_envelope_t, input_envelope_queue)
DEFINE_LOCKFREE_QUEUE(output_msg_envelope_t, output_envelope_queue)

bool processor_init(processor_context_t* ctx,
                   const processor_config_t* config,
                   matching_engine_t* engine,
                   input_envelope_queue_t* input_queue,
                   output_envelope_queue_t* output_queue,
                   atomic_bool* shutdown_flag) {
    memset(ctx, 0, sizeof(*ctx));
    
    ctx->config = *config;
    ctx->engine = engine;
    ctx->input_queue = input_queue;
    ctx->output_queue = output_queue;
    ctx->shutdown_flag = shutdown_flag;
    
    return true;
}

void processor_cleanup(processor_context_t* ctx) {
    (void)ctx;
}

void* processor_thread(void* arg) {
    processor_context_t* ctx = (processor_context_t*)arg;
    
    fprintf(stderr, "[Processor] Starting (mode: %s)\n",
            ctx->config.tcp_mode ? "TCP" : "UDP");
    
    input_msg_envelope_t input_batch[BATCH_SIZE];
    output_msg_t output_buffer[1024];
    
    while (!atomic_load(ctx->shutdown_flag)) {
        // Dequeue batch of input messages
        size_t count = 0;
        for (size_t i = 0; i < BATCH_SIZE; i++) {
            if (input_envelope_queue_dequeue(ctx->input_queue, &input_batch[count])) {
                count++;
            } else {
                break;
            }
        }
        
        if (count == 0) {
            usleep(SLEEP_TIME_US);
            continue;
        }
        
        ctx->batches_processed++;
        
        // Process each message
        for (size_t i = 0; i < count; i++) {
            input_msg_envelope_t* envelope = &input_batch[i];
            input_msg_t* msg = &envelope->msg;
            uint32_t client_id = envelope->client_id;
            
            size_t output_count = 0;
            
            switch (msg->type) {
                case INPUT_NEW_ORDER: {
                    new_order_input_t* order_input = &msg->data.new_order;
                    output_count = matching_engine_add_order(
                        ctx->engine,
                        order_input->user_id,
                        order_input->symbol,
                        order_input->price,
                        order_input->quantity,
                        order_input->side,
                        order_input->user_order_id,
                        client_id,  // Pass client_id for ownership tracking
                        output_buffer
                    );
                    break;
                }
                
                case INPUT_CANCEL: {
                    cancel_input_t* cancel_input = &msg->data.cancel;
                    output_count = matching_engine_cancel_order(
                        ctx->engine,
                        cancel_input->user_id,
                        cancel_input->user_order_id,
                        output_buffer
                    );
                    break;
                }
                
                case INPUT_FLUSH: {
                    output_count = matching_engine_flush(ctx->engine, output_buffer);
                    break;
                }
            }
            
            // Wrap outputs in envelopes with routing logic
            for (size_t j = 0; j < output_count; j++) {
                output_msg_t* out_msg = &output_buffer[j];
                
                if (out_msg->type == OUTPUT_TRADE) {
                    // Trade - route to BOTH participants
                    uint32_t buy_client = out_msg->data.trade.buy_client_id;
                    uint32_t sell_client = out_msg->data.trade.sell_client_id;
                    
                    // Send to buyer
                    output_msg_envelope_t env1 = create_output_envelope(out_msg, buy_client);
                    if (!output_envelope_queue_enqueue(ctx->output_queue, &env1)) {
                        fprintf(stderr, "[Processor] Output queue full!\n");
                    }
                    
                    // Send to seller if different client
                    if (buy_client != sell_client) {
                        output_msg_envelope_t env2 = create_output_envelope(out_msg, sell_client);
                        if (!output_envelope_queue_enqueue(ctx->output_queue, &env2)) {
                            fprintf(stderr, "[Processor] Output queue full!\n");
                        }
                    }
                } else {
                    // Ack, Cancel, TOB - route to originating client
                    output_msg_envelope_t envelope = create_output_envelope(out_msg, client_id);
                    if (!output_envelope_queue_enqueue(ctx->output_queue, &envelope)) {
                        fprintf(stderr, "[Processor] Output queue full!\n");
                    }
                }
            }
            
            ctx->messages_processed++;
        }
    }
    
    fprintf(stderr, "[Processor] Shutting down\n");
    processor_print_stats(ctx);
    
    return NULL;
}

void processor_print_stats(const processor_context_t* ctx) {
    fprintf(stderr, "\n=== Processor Statistics ===\n");
    fprintf(stderr, "Messages processed:    %lu\n", ctx->messages_processed);
    fprintf(stderr, "Batches processed:     %lu\n", ctx->batches_processed);
}

void processor_cancel_client_orders(processor_context_t* ctx, uint32_t client_id) {
    // TODO: Walk through all order books and cancel orders for this client
    fprintf(stderr, "[Processor] Cancelling all orders for client %u (TODO)\n", client_id);
    (void)ctx;
}

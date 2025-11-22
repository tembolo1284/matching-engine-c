#include "threading/processor.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const struct timespec ts = {
    .tv_sec = 0,
    .tv_nsec = 1000
};

bool processor_init(processor_t* processor,
                    const processor_config_t* config,
                    matching_engine_t* engine,
                    input_envelope_queue_t* input_queue,
                    output_envelope_queue_t* output_queue,
                    atomic_bool* shutdown_flag) {
    memset(processor, 0, sizeof(*processor));
    
    processor->config = *config;
    processor->engine = engine;
    processor->input_queue = input_queue;
    processor->output_queue = output_queue;
    processor->shutdown_flag = shutdown_flag;
    
    atomic_init(&processor->running, false);
    atomic_init(&processor->started, false);
    atomic_init(&processor->messages_processed, 0);
    atomic_init(&processor->batches_processed, 0);
    atomic_init(&processor->output_sequence, 0);
    
    return true;
}

void processor_cleanup(processor_t* processor) {
    (void)processor;  // Nothing to clean up for now
}

void* processor_thread(void* arg) {
    processor_t* processor = (processor_t*)arg;
    
    fprintf(stderr, "[Processor] Starting (mode: %s)\n",
            processor->config.tcp_mode ? "TCP" : "UDP");
    
    input_msg_envelope_t input_batch[PROCESSOR_BATCH_SIZE];
    output_buffer_t output_buffer;
    
    while (!atomic_load(processor->shutdown_flag)) {
        // Dequeue batch of input messages
        size_t count = 0;
        for (size_t i = 0; i < PROCESSOR_BATCH_SIZE; i++) {
            if (input_envelope_queue_dequeue(processor->input_queue, &input_batch[count])) {
                count++;
            } else {
                break;
            }
        }
        
        if (count == 0) {
            nanosleep(&ts, NULL);
            continue;
        }
        
        atomic_fetch_add(&processor->batches_processed, 1);
        
        // Process each message
        for (size_t i = 0; i < count; i++) {
            input_msg_envelope_t* envelope = &input_batch[i];
            input_msg_t* msg = &envelope->msg;
            uint32_t client_id = envelope->client_id;
            
            // Initialize output buffer
            output_buffer_init(&output_buffer);
            
            // Process through matching engine
            matching_engine_process_message(processor->engine, msg, client_id, &output_buffer);
            
            // Wrap outputs in envelopes with routing logic
            for (int j = 0; j < output_buffer.count; j++) {
                output_msg_t* out_msg = &output_buffer.messages[j];
                uint64_t seq = atomic_fetch_add(&processor->output_sequence, 1);
                
                if (out_msg->type == OUTPUT_MSG_TRADE) {
                    // Trade - route to BOTH participants
                    uint32_t buy_client = out_msg->data.trade.buy_client_id;
                    uint32_t sell_client = out_msg->data.trade.sell_client_id;
                    
                    // Send to buyer
                    output_msg_envelope_t env1 = create_output_envelope(out_msg, buy_client, seq);
                    if (!output_envelope_queue_enqueue(processor->output_queue, &env1)) {
                        fprintf(stderr, "[Processor] Output queue full!\n");
                    }
                    
                    // Send to seller if different client
                    if (buy_client != sell_client) {
                        output_msg_envelope_t env2 = create_output_envelope(out_msg, sell_client, seq);
                        if (!output_envelope_queue_enqueue(processor->output_queue, &env2)) {
                            fprintf(stderr, "[Processor] Output queue full!\n");
                        }
                    }
                } else {
                    // Ack, Cancel, TOB - route to originating client
                    output_msg_envelope_t env = create_output_envelope(out_msg, client_id, seq);
                    if (!output_envelope_queue_enqueue(processor->output_queue, &env)) {
                        fprintf(stderr, "[Processor] Output queue full!\n");
                    }
                }
            }
            
            atomic_fetch_add(&processor->messages_processed, 1);
        }
    }
    
    fprintf(stderr, "[Processor] Shutting down\n");
    processor_print_stats(processor);
    return NULL;
}

void processor_print_stats(const processor_t* processor) {
    fprintf(stderr, "\n=== Processor Statistics ===\n");
    fprintf(stderr, "Messages processed:    %llu\n", 
           (unsigned long long) atomic_load(&processor->messages_processed));
    fprintf(stderr, "Batches processed:     %llu\n", 
           (unsigned long long)atomic_load(&processor->batches_processed));
}

void processor_cancel_client_orders(processor_t* processor, uint32_t client_id) {
    fprintf(stderr, "[Processor] Cancelling all orders for client %u\n", client_id);
    
    // Initialize output buffer for cancel acknowledgements
    output_buffer_t output_buffer;
    output_buffer_init(&output_buffer);
    
    // Call matching engine to cancel all orders for this client
    size_t cancelled = matching_engine_cancel_client_orders(
        processor->engine,
        client_id,
        &output_buffer
    );
    
    fprintf(stderr, "[Processor] Cancelled %zu orders for client %u\n", 
            cancelled, client_id);
    
    // Enqueue cancel acknowledgements to output queue
    for (int i = 0; i < output_buffer.count; i++) {
        output_msg_t* out_msg = &output_buffer.messages[i];
        uint64_t seq = atomic_fetch_add(&processor->output_sequence, 1);
        
        // Route cancel acks back to the disconnected client (for logging/cleanup)
        output_msg_envelope_t envelope = create_output_envelope(out_msg, client_id, seq);
        
        if (!output_envelope_queue_enqueue(processor->output_queue, &envelope)) {
            fprintf(stderr, "[Processor] Output queue full while cancelling client orders!\n");
        }
    }
}

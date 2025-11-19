#include "threading/processor.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/**
 * Sleep for microseconds
 */
static void usleep_safe(unsigned int usec) {
    struct timespec ts;
    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (usec % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

/**
 * Initialize processor
 */
void processor_init(processor_t* processor, 
                    input_queue_t* input_queue,
                    output_queue_t* output_queue) {
    processor->input_queue = input_queue;
    processor->output_queue = output_queue;
    
    matching_engine_init(&processor->engine);
    
    atomic_init(&processor->running, false);
    atomic_init(&processor->started, false);
    atomic_init(&processor->messages_processed, 0);
    atomic_init(&processor->batches_processed, 0);
}

/**
 * Destroy processor and cleanup resources
 */
void processor_destroy(processor_t* processor) {
    processor_stop(processor);
    matching_engine_destroy(&processor->engine);
}

/**
 * Process a single input message
 */
void processor_process_message(processor_t* processor, const input_msg_t* msg) {
    // Create output buffer
    output_buffer_t output;
    output_buffer_init(&output);
    
    // Process through matching engine
    matching_engine_process_message(&processor->engine, msg, &output);
    
    // Push all output messages to output queue
    for (int i = 0; i < output.count; i++) {
        int retry_count = 0;
        const int MAX_RETRIES = 1000;
        
        while (!output_queue_push(processor->output_queue, &output.messages[i])) {
            retry_count++;
            if (retry_count >= MAX_RETRIES) {
                fprintf(stderr, "WARNING: Output queue full, dropping message!\n");
                break;
            }
            // Very brief wait
            sched_yield();
        }
    }
}

/**
 * Process a batch of messages
 * Returns number of messages processed
 */
size_t processor_process_batch(processor_t* processor) {
    size_t processed = 0;
    
    for (size_t i = 0; i < PROCESSOR_BATCH_SIZE; i++) {
        input_msg_t msg;
        
        if (!input_queue_pop(processor->input_queue, &msg)) {
            break;  // Queue empty, exit batch
        }
        
        processor_process_message(processor, &msg);
        processed++;
    }
    
    return processed;
}

/**
 * Thread entry point
 */
void* processor_thread_func(void* arg) {
    processor_t* processor = (processor_t*)arg;
    
    fprintf(stderr, "Processor thread started\n");
    
    size_t empty_iterations = 0;
    
    while (atomic_load_explicit(&processor->running, memory_order_acquire)) {
        // Try to process a batch of messages
        size_t processed = processor_process_batch(processor);
        
        if (processed > 0) {
            // Processed some messages
            atomic_fetch_add_explicit(&processor->messages_processed, processed, memory_order_relaxed);
            atomic_fetch_add_explicit(&processor->batches_processed, 1, memory_order_relaxed);
            empty_iterations = 0;  // Reset counter
        } else {
            // Queue was empty
            empty_iterations++;
            
            // Adaptive sleep - sleep longer if consistently empty
            if (empty_iterations > PROCESSOR_IDLE_THRESHOLD) {
                // Been empty for a while, sleep a bit longer
                usleep_safe(PROCESSOR_IDLE_SLEEP_US);
            } else {
                // Just became empty, use very short sleep
                usleep_safe(PROCESSOR_ACTIVE_SLEEP_US);
            }
        }
    }
    
    // Drain remaining messages in queue before exiting
    fprintf(stderr, "Draining remaining input messages...\n");
    size_t drained = 0;
    
    while (true) {
        input_msg_t msg;
        if (!input_queue_pop(processor->input_queue, &msg)) {
            break;
        }
        
        processor_process_message(processor, &msg);
        drained++;
    }
    
    if (drained > 0) {
        fprintf(stderr, "Drained %zu messages from input queue\n", drained);
        atomic_fetch_add(&processor->messages_processed, drained);
    }
    
    fprintf(stderr, "Processor thread stopped. Messages processed: %lu, Batches: %lu\n",
            atomic_load(&processor->messages_processed),
            atomic_load(&processor->batches_processed));
    
    return NULL;
}

/**
 * Start processing (spawns thread)
 */
bool processor_start(processor_t* processor) {
    // Check if already running
    bool expected = false;
    if (!atomic_compare_exchange_strong(&processor->started, &expected, true)) {
        return false;  // Already started
    }
    
    atomic_store_explicit(&processor->running, true, memory_order_release);
    
    // Create thread
    int result = pthread_create(&processor->thread, NULL, processor_thread_func, processor);
    if (result != 0) {
        fprintf(stderr, "ERROR: Failed to create processor thread: %s\n", strerror(result));
        atomic_store(&processor->running, false);
        atomic_store(&processor->started, false);
        return false;
    }
    
    return true;
}

/**
 * Stop processing (signals thread to exit and waits)
 */
void processor_stop(processor_t* processor) {
    // Check if started
    if (!atomic_load(&processor->started)) {
        return;
    }
    
    // Signal thread to stop
    atomic_store_explicit(&processor->running, false, memory_order_release);
    
    // Wait for thread to finish
    pthread_join(processor->thread, NULL);
    
    atomic_store(&processor->started, false);
}

/**
 * Check if thread is running
 */
bool processor_is_running(const processor_t* processor) {
    return atomic_load_explicit(&processor->running, memory_order_acquire);
}

/**
 * Get statistics
 */
uint64_t processor_get_messages_processed(const processor_t* processor) {
    return atomic_load(&processor->messages_processed);
}

uint64_t processor_get_batches_processed(const processor_t* processor) {
    return atomic_load(&processor->batches_processed);
}

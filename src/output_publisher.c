#include "output_publisher.h"
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
 * Initialize output publisher
 */
void output_publisher_init(output_publisher_t* publisher, output_queue_t* input_queue) {
    publisher->input_queue = input_queue;
    
    message_formatter_init(&publisher->formatter);
    
    atomic_init(&publisher->running, false);
    atomic_init(&publisher->started, false);
    atomic_init(&publisher->messages_published, 0);
}

/**
 * Destroy output publisher and cleanup resources
 */
void output_publisher_destroy(output_publisher_t* publisher) {
    output_publisher_stop(publisher);
}

/**
 * Publish a single output message to stdout
 */
void output_publisher_publish_message(output_publisher_t* publisher, const output_msg_t* msg) {
    // Format message to string
    const char* output = message_formatter_format(&publisher->formatter, msg);
    
    // Write to stdout
    printf("%s\n", output);
    
    // Flush to ensure real-time output
    fflush(stdout);
}

/**
 * Thread entry point
 */
void* output_publisher_thread_func(void* arg) {
    output_publisher_t* publisher = (output_publisher_t*)arg;
    
    fprintf(stderr, "Output Publisher thread started\n");
    
    while (atomic_load_explicit(&publisher->running, memory_order_acquire)) {
        output_msg_t msg;
        
        if (output_queue_pop(publisher->input_queue, &msg)) {
            // Got a message - publish it
            output_publisher_publish_message(publisher, &msg);
            atomic_fetch_add_explicit(&publisher->messages_published, 1, memory_order_relaxed);
        } else {
            // Queue empty - brief sleep to avoid busy-waiting
            usleep_safe(OUTPUT_SLEEP_US);
        }
    }
    
    // Drain remaining messages in queue before exiting
    fprintf(stderr, "Draining remaining output messages...\n");
    size_t drained = 0;
    
    while (true) {
        output_msg_t msg;
        if (!output_queue_pop(publisher->input_queue, &msg)) {
            break;
        }
        
        output_publisher_publish_message(publisher, &msg);
        drained++;
    }
    
    if (drained > 0) {
        fprintf(stderr, "Drained %zu messages from output queue\n", drained);
        atomic_fetch_add(&publisher->messages_published, drained);
    }
    
    fprintf(stderr, "Output Publisher thread stopped. Messages published: %lu\n",
            atomic_load(&publisher->messages_published));
    
    return NULL;
}

/**
 * Start publishing (spawns thread)
 */
bool output_publisher_start(output_publisher_t* publisher) {
    // Check if already running
    bool expected = false;
    if (!atomic_compare_exchange_strong(&publisher->started, &expected, true)) {
        return false;  // Already started
    }
    
    atomic_store_explicit(&publisher->running, true, memory_order_release);
    
    // Create thread
    int result = pthread_create(&publisher->thread, NULL, output_publisher_thread_func, publisher);
    if (result != 0) {
        fprintf(stderr, "ERROR: Failed to create output publisher thread: %s\n", strerror(result));
        atomic_store(&publisher->running, false);
        atomic_store(&publisher->started, false);
        return false;
    }
    
    return true;
}

/**
 * Stop publishing (signals thread to exit, drains queue, and waits)
 */
void output_publisher_stop(output_publisher_t* publisher) {
    // Check if started
    if (!atomic_load(&publisher->started)) {
        return;
    }
    
    // Signal thread to stop
    atomic_store_explicit(&publisher->running, false, memory_order_release);
    
    // Wait for thread to finish
    pthread_join(publisher->thread, NULL);
    
    atomic_store(&publisher->started, false);
}

/**
 * Check if thread is running
 */
bool output_publisher_is_running(const output_publisher_t* publisher) {
    return atomic_load_explicit(&publisher->running, memory_order_acquire);
}

/**
 * Get statistics
 */
uint64_t output_publisher_get_messages_published(const output_publisher_t* publisher) {
    return atomic_load(&publisher->messages_published);
}

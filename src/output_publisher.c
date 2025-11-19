#include "output_publisher.h"
#include "binary_message_formatter.h"
#include "binary_protocol.h"
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
void output_publisher_init(output_publisher_t* publisher, 
                           output_queue_t* output_queue,
                           bool use_binary) {
    publisher->output_queue = output_queue;
    message_formatter_init(&publisher->csv_formatter);
    binary_message_formatter_init(&publisher->binary_formatter);
    publisher->use_binary = use_binary;

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
 * Thread entry point for output publisher
 */
void* output_publisher_thread_func(void* arg) {
    output_publisher_t* publisher = (output_publisher_t*)arg;

    fprintf(stderr, "Output Publisher thread started\n");
    if (publisher->use_binary) {
        fprintf(stderr, "Output mode: BINARY\n");
    } else {
        fprintf(stderr, "Output mode: CSV\n");
    }

    size_t empty_iterations = 0;

    while (atomic_load_explicit(&publisher->running, memory_order_acquire)) {
        output_msg_t msg;

        if (output_queue_pop(publisher->output_queue, &msg)) {
            // Got a message - publish it
            if (publisher->use_binary) {
                /* Binary output */
                size_t bin_len;
                const void* bin_data = binary_message_formatter_format(
                    &publisher->binary_formatter,
                    &msg,
                    &bin_len
                );

                if (bin_data && bin_len > 0) {
                    /* Write binary data to stdout */
                    size_t written = fwrite(bin_data, 1, bin_len, stdout);
                    if (written != bin_len) {
                        fprintf(stderr, "WARNING: Incomplete binary write: %zu/%zu bytes\n",
                                written, bin_len);
                    }
                    fflush(stdout);
                }
            } else {
                /* CSV output */
                const char* formatted = message_formatter_format(
                    &publisher->csv_formatter,
                    &msg
                );
                printf("%s\n", formatted);
                fflush(stdout);
            }

            atomic_fetch_add_explicit(&publisher->messages_published, 1, memory_order_relaxed);
            empty_iterations = 0;  // Reset counter
        } else {
            // Queue was empty
            empty_iterations++;

            // Adaptive sleep - sleep longer if consistently empty
            if (empty_iterations > OUTPUT_IDLE_THRESHOLD) {
                // Been empty for a while, sleep a bit longer
                usleep_safe(OUTPUT_IDLE_SLEEP_US);
            } else {
                // Just became empty, use very short sleep
                usleep_safe(OUTPUT_ACTIVE_SLEEP_US);
            }
        }
    }

    // Drain remaining messages in queue before exiting
    fprintf(stderr, "Draining remaining output messages...\n");
    size_t drained = 0;

    while (true) {
        output_msg_t msg;
        if (!output_queue_pop(publisher->output_queue, &msg)) {
            break;
        }

        // Publish the message
        if (publisher->use_binary) {
            /* Binary output */
            size_t bin_len;
            const void* bin_data = binary_message_formatter_format(
                &publisher->binary_formatter,
                &msg,
                &bin_len
            );

            if (bin_data && bin_len > 0) {
                fwrite(bin_data, 1, bin_len, stdout);
                fflush(stdout);
            }
        } else {
            /* CSV output */
            const char* formatted = message_formatter_format(
                &publisher->csv_formatter,
                &msg
            );
            printf("%s\n", formatted);
            fflush(stdout);
        }

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

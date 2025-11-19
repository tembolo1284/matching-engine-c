#include "udp_receiver.h"
#include "processor.h"
#include "output_publisher.h"
#include "queues.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

/* ============================================================================
 * Global shutdown flag
 * ============================================================================ */

static atomic_bool shutdown_requested = ATOMIC_VAR_INIT(false);

/**
 * Signal handler for graceful shutdown
 */
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        fprintf(stderr, "\nShutdown signal received...\n");
        atomic_store(&shutdown_requested, true);
    }
}

/**
 * Sleep for milliseconds
 */
static void msleep(unsigned int msec) {
    struct timespec ts;
    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

/* ============================================================================
 * Main entry point
 * ============================================================================ */

int main(int argc, char* argv[]) {
    fprintf(stderr, "DEBUG: Entering main\n");
    fflush(stderr);
    
    // Register signal handlers for graceful shutdown
    fprintf(stderr, "DEBUG: Registering signal handlers\n");
    fflush(stderr);

    // Register signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    fprintf(stderr, "DEBUG: Parsing arguments\n");
    fflush(stderr);
    
    // Parse command line arguments (optional: port number)
    uint16_t port = 1234;
    if (argc > 1) {
        long parsed_port = strtol(argv[1], NULL, 10);
        if (parsed_port > 0 && parsed_port <= 65535) {
            port = (uint16_t)parsed_port;
        } else {
            fprintf(stderr, "Invalid port number, using default: 1234\n");
        }
    }

    fprintf(stderr, "DEBUG: About to print banner\n");
    fflush(stderr);
    
    fprintf(stderr, "==============================================================\n");
    fprintf(stderr, "Matching Engine - Multi-threaded Matching Engine (C Port)\n");
    fprintf(stderr, "==============================================================\n");
    fprintf(stderr, "UDP Port: %u\n", port);
    fprintf(stderr, "==============================================================\n");
    fprintf(stderr, "DEBUG: About to allocate queues\n");
    fflush(stderr);    

    // Create lock-free queues
    input_queue_t* input_queue = malloc(sizeof(input_queue_t));
    output_queue_t* output_queue = malloc(sizeof(output_queue_t));
    
    if (!input_queue || !output_queue) {
        fprintf(stderr, "ERROR: Failed to allocate queues\n");
        return 1;
    }

    input_queue_init(input_queue);
    output_queue_init(output_queue);
    
    fprintf(stderr, "Queue Configuration:\n");
    fprintf(stderr, "  Input queue capacity:  %zu messages\n", input_queue_capacity(input_queue));
    fprintf(stderr, "  Output queue capacity: %zu messages\n", output_queue_capacity(output_queue));
    fprintf(stderr, "==============================================================\n");
    
    // Create thread components
    fprintf(stderr, "DEBUG: Allocating thread components (heap)\n");
    fflush(stderr);
    
    // HEAP ALLOCATE - Components are too large for stack
    udp_receiver_t* receiver = malloc(sizeof(udp_receiver_t));
    processor_t* processor = malloc(sizeof(processor_t));
    output_publisher_t* publisher = malloc(sizeof(output_publisher_t));
    
    if (!receiver || !processor || !publisher) {
        fprintf(stderr, "ERROR: Failed to allocate thread components\n");
        free(input_queue);
        free(output_queue);
        return 1;
    }
    
    fprintf(stderr, "DEBUG: Initializing components\n");
    fflush(stderr);
    
    udp_receiver_init(receiver, input_queue, port);
    processor_init(processor, input_queue, output_queue);
    output_publisher_init(publisher, output_queue);
    
    // Start all threads
    fprintf(stderr, "Starting threads...\n");

    if (!udp_receiver_start(receiver)) {
        fprintf(stderr, "ERROR: Failed to start UDP receiver\n");
        free(receiver);
        free(processor);
        free(publisher);
        free(input_queue);
        free(output_queue);
        return 1;
    }
    
    if (!processor_start(processor)) {
        fprintf(stderr, "ERROR: Failed to start processor\n");
        udp_receiver_stop(receiver);
        free(receiver);
        free(processor);
        free(publisher);
        free(input_queue);
        free(output_queue);
        return 1;
    }
    
    if (!output_publisher_start(publisher)) {
        fprintf(stderr, "ERROR: Failed to start output publisher\n");
        processor_stop(processor);
        udp_receiver_stop(receiver);
        free(receiver);
        free(processor);
        free(publisher);
        free(input_queue);
        free(output_queue);
        return 1;
    }   
 
    fprintf(stderr, "All threads started. System is running.\n");
    fprintf(stderr, "Press Ctrl+C to shutdown gracefully.\n");
    fprintf(stderr, "==============================================================\n");
    
    // Main thread waits for shutdown signal
    while (!atomic_load(&shutdown_requested)) {
        msleep(50);
        
        // Optional: Monitor queue depths (useful for debugging)
        // Uncomment to see queue usage in real-time
        /*
        size_t input_size = input_queue_size(&input_queue);
        size_t output_size = output_queue_size(&output_queue);
        if (input_size > 1000 || output_size > 1000) {
            fprintf(stderr, "Queue depths - Input: %zu, Output: %zu\n", 
                    input_size, output_size);
        }
        */
    }
    
    // Graceful shutdown
    fprintf(stderr, "==============================================================\n");
    fprintf(stderr, "Initiating graceful shutdown...\n");
    
    // Stop receiver first (no more input)
    fprintf(stderr, "Stopping UDP receiver...\n");
    udp_receiver_stop(receiver);
    
    // Give processor time to drain input queue
    fprintf(stderr, "Draining input queue (size: %zu)...\n", input_queue_size(input_queue));
    msleep(200);
    
    // Stop processor (no more processing)
    fprintf(stderr, "Stopping processor...\n");
    processor_stop(processor);
    
    // Give publisher time to drain output queue
    fprintf(stderr, "Draining output queue (size: %zu)...\n", output_queue_size(output_queue));
    msleep(200);
    
    // Stop publisher (no more output)
    fprintf(stderr, "Stopping output publisher...\n");
    output_publisher_stop(publisher);
    
    // Print statistics
    fprintf(stderr, "==============================================================\n");
    fprintf(stderr, "Final Statistics:\n");
    fprintf(stderr, "  Packets received:    %lu\n", udp_receiver_get_packets_received(receiver));
    fprintf(stderr, "  Messages parsed:     %lu\n", udp_receiver_get_messages_parsed(receiver));
    fprintf(stderr, "  Messages processed:  %lu\n", processor_get_messages_processed(processor));
    fprintf(stderr, "  Batches processed:   %lu\n", processor_get_batches_processed(processor));
    fprintf(stderr, "  Messages published:  %lu\n", output_publisher_get_messages_published(publisher));
    fprintf(stderr, "==============================================================\n");
    
    // Cleanup
    udp_receiver_destroy(receiver);
    processor_destroy(processor);
    output_publisher_destroy(publisher);
    free(receiver);
    free(processor);
    free(publisher);
    free(input_queue);
    free(output_queue);

    fprintf(stderr, "Shutdown complete. Goodbye!\n");
    fprintf(stderr, "==============================================================\n");
    
    return 0;
}

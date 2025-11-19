#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include "network/udp_receiver.h"
#include "threading/processor.h"
#include "threading/output_publisher.h"
#include "threading/queues.h"

/* Global components for signal handler */
static udp_receiver_t* g_udp_receiver = NULL;
static processor_t* g_processor = NULL;
static output_publisher_t* g_output_publisher = NULL;

/**
 * Signal handler for graceful shutdown
 */
void signal_handler(int signum) {
    fprintf(stderr, "\nReceived signal %d, shutting down gracefully...\n", signum);

    if (g_udp_receiver) {
        udp_receiver_stop(g_udp_receiver);
    }
    if (g_processor) {
        processor_stop(g_processor);
    }
    if (g_output_publisher) {
        output_publisher_stop(g_output_publisher);
    }
}

/**
 * Print usage information
 */
void print_usage(const char* program_name) {
    fprintf(stderr, "Usage: %s [port] [--binary]\n", program_name);
    fprintf(stderr, "  port      : UDP port to listen on (default: 1234)\n");
    fprintf(stderr, "  --binary  : Use binary protocol for output (default: CSV)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s              # Listen on port 1234, CSV output\n", program_name);
    fprintf(stderr, "  %s 5000         # Listen on port 5000, CSV output\n", program_name);
    fprintf(stderr, "  %s 1234 --binary # Listen on port 1234, binary output\n", program_name);
    fprintf(stderr, "\n");
}

int main(int argc, char* argv[]) {
    fprintf(stderr, "=== Matching Engine Starting ===\n");

    /* Parse command line arguments */
    int port = 1234;  // Default port
    bool use_binary = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--binary") == 0) {
            use_binary = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            // Try to parse as port number
            int parsed_port = atoi(argv[i]);
            if (parsed_port > 0 && parsed_port <= 65535) {
                port = parsed_port;
            } else {
                fprintf(stderr, "ERROR: Invalid port number: %s\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
        }
    }

    fprintf(stderr, "Configuration:\n");
    fprintf(stderr, "  Port: %d\n", port);
    fprintf(stderr, "  Protocol: %s (input auto-detects both CSV and binary)\n", 
            use_binary ? "BINARY" : "CSV");
    fprintf(stderr, "\n");

    /* Setup signal handlers for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Allocate queues */
    input_queue_t* input_queue = (input_queue_t*)malloc(sizeof(input_queue_t));
    output_queue_t* output_queue = (output_queue_t*)malloc(sizeof(output_queue_t));

    if (!input_queue || !output_queue) {
        fprintf(stderr, "ERROR: Failed to allocate queues\n");
        free(input_queue);
        free(output_queue);
        return 1;
    }

    /* Initialize queues */
    input_queue_init(input_queue);
    output_queue_init(output_queue);

    /* Allocate components on heap (they're too big for stack!) */
    udp_receiver_t* udp_receiver = (udp_receiver_t*)calloc(1, sizeof(udp_receiver_t));
    processor_t* processor = (processor_t*)calloc(1, sizeof(processor_t));
    output_publisher_t* output_publisher = (output_publisher_t*)calloc(1, sizeof(output_publisher_t));

    if (!udp_receiver || !processor || !output_publisher) {
        fprintf(stderr, "ERROR: Failed to allocate components\n");
        free(udp_receiver);
        free(processor);
        free(output_publisher);
        free(input_queue);
        free(output_queue);
        return 1;
    }

    /* Initialize components */
    udp_receiver_init(udp_receiver, input_queue, port);
    processor_init(processor, input_queue, output_queue);
    output_publisher_init(output_publisher, output_queue, use_binary);

    /* Set global pointers for signal handler */
    g_udp_receiver = udp_receiver;
    g_processor = processor;
    g_output_publisher = output_publisher;

    /* Start all threads */
    fprintf(stderr, "Starting threads...\n");

    if (!udp_receiver_start(udp_receiver)) {
        fprintf(stderr, "ERROR: Failed to start UDP receiver\n");
        goto cleanup;
    }

    if (!processor_start(processor)) {
        fprintf(stderr, "ERROR: Failed to start processor\n");
        udp_receiver_stop(udp_receiver);
        goto cleanup;
    }

    if (!output_publisher_start(output_publisher)) {
        fprintf(stderr, "ERROR: Failed to start output publisher\n");
        udp_receiver_stop(udp_receiver);
        processor_stop(processor);
        goto cleanup;
    }

    fprintf(stderr, "All threads started successfully!\n");
    fprintf(stderr, "Listening for orders on UDP port %d...\n", port);
    fprintf(stderr, "Press Ctrl+C to stop.\n");
    fprintf(stderr, "\n");

    /* Wait for threads to complete (will happen on signal) */
    while (udp_receiver_is_running(udp_receiver) ||
           processor_is_running(processor) ||
           output_publisher_is_running(output_publisher)) {
        sleep(1);
    }

    fprintf(stderr, "\n=== Final Statistics ===\n");
    fprintf(stderr, "UDP Receiver:\n");
    fprintf(stderr, "  Packets received: %lu\n", udp_receiver_get_packets_received(udp_receiver));
    fprintf(stderr, "  Messages parsed:  %lu\n", udp_receiver_get_messages_parsed(udp_receiver));
    fprintf(stderr, "  Messages dropped: %lu\n", udp_receiver_get_messages_dropped(udp_receiver));
    fprintf(stderr, "\n");
    fprintf(stderr, "Processor:\n");
    fprintf(stderr, "  Messages processed: %lu\n", processor_get_messages_processed(processor));
    fprintf(stderr, "  Batches processed:  %lu\n", processor_get_batches_processed(processor));
    fprintf(stderr, "\n");
    fprintf(stderr, "Output Publisher:\n");
    fprintf(stderr, "  Messages published: %lu\n", output_publisher_get_messages_published(output_publisher));
    fprintf(stderr, "\n");

cleanup:
    /* Clean up components */
    udp_receiver_destroy(udp_receiver);
    processor_destroy(processor);
    output_publisher_destroy(output_publisher);

    /* Free components */
    free(udp_receiver);
    free(processor);
    free(output_publisher);

    /* Free queues */
    free(input_queue);
    free(output_queue);

    /* Clear global pointers */
    g_udp_receiver = NULL;
    g_processor = NULL;
    g_output_publisher = NULL;

    fprintf(stderr, "=== Matching Engine Stopped ===\n");

    return 0;
}

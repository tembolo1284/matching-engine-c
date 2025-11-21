#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include "core/matching_engine.h"
#include "protocol/message_types_extended.h"
#include "network/udp_receiver.h"
#include "network/tcp_listener.h"
#include "network/tcp_connection.h"
#include "threading/processor.h"
#include "threading/output_publisher.h"
#include "threading/output_router.h"

// Global shutdown flag
static atomic_bool g_shutdown = false;

// Signal handler for graceful shutdown
static void signal_handler(int signum) {
    fprintf(stderr, "\nReceived signal %d, shutting down gracefully...\n", signum);
    atomic_store(&g_shutdown, true);
}

// Usage information
static void print_usage(const char* program_name) {
    fprintf(stderr, "Usage: %s [options]\n", program_name);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --tcp [port]     Use TCP mode (default port: 1234)\n");
    fprintf(stderr, "  --udp [port]     Use UDP mode (default port: 1234)\n");
    fprintf(stderr, "  --binary         Use binary protocol for output\n");
    fprintf(stderr, "  --help           Display this help message\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s --tcp                # TCP on port 1234, CSV output\n", program_name);
    fprintf(stderr, "  %s --tcp 5000           # TCP on port 5000\n", program_name);
    fprintf(stderr, "  %s --tcp --binary       # TCP with binary output\n", program_name);
    fprintf(stderr, "  %s --udp                # UDP on port 1234 (legacy mode)\n", program_name);
    fprintf(stderr, "  %s --udp 5000 --binary  # UDP on port 5000, binary output\n", program_name);
}

// Configuration
typedef struct {
    bool tcp_mode;              // true = TCP, false = UDP
    uint16_t port;              // Network port
    bool binary_output;         // true = binary, false = CSV
} app_config_t;

// Parse command line arguments
static bool parse_args(int argc, char** argv, app_config_t* config) {
    // Defaults
    config->tcp_mode = true;    // Default to TCP
    config->port = 1234;
    config->binary_output = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tcp") == 0) {
            config->tcp_mode = true;
            // Check if next arg is a port number
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->port = (uint16_t)atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "--udp") == 0) {
            config->tcp_mode = false;
            // Check if next arg is a port number
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->port = (uint16_t)atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "--binary") == 0) {
            config->binary_output = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return false;
        } else if (argv[i][0] != '-') {
            // Positional argument - treat as port for backward compatibility
            config->port = (uint16_t)atoi(argv[i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return false;
        }
    }

    return true;
}

// TCP mode main function
static int run_tcp_mode(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - TCP Mode\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "Max clients:    %d\n", MAX_TCP_CLIENTS);
    fprintf(stderr, "========================================\n\n");

    // Allocate matching engine on heap
    matching_engine_t* engine = malloc(sizeof(matching_engine_t));
    if (!engine) {
        fprintf(stderr, "Failed to allocate matching engine\n");
        return 1;
    }
    matching_engine_init(engine);

    // Allocate client registry on heap
    tcp_client_registry_t* client_registry = malloc(sizeof(tcp_client_registry_t));
    if (!client_registry) {
        fprintf(stderr, "Failed to allocate client registry\n");
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }
    tcp_client_registry_init(client_registry);

    // Allocate queues on heap
    input_envelope_queue_t* input_queue = malloc(sizeof(input_envelope_queue_t));
    if (!input_queue) {
        fprintf(stderr, "Failed to allocate input queue\n");
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }
    input_envelope_queue_init(input_queue);

    output_envelope_queue_t* output_queue = malloc(sizeof(output_envelope_queue_t));
    if (!output_queue) {
        fprintf(stderr, "Failed to allocate output queue\n");
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }
    output_envelope_queue_init(output_queue);

    // Thread contexts (these are smaller, can stay on stack)
    tcp_listener_context_t listener_ctx;
    processor_t processor_ctx;
    output_router_context_t router_ctx;

    // Configure TCP listener
    tcp_listener_config_t listener_config = {
        .port = config->port,
        .listen_backlog = 10,
        .use_binary_output = config->binary_output
    };

    if (!tcp_listener_init(&listener_ctx, &listener_config, client_registry,
                           input_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize TCP listener\n");
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    // Configure processor
    processor_config_t processor_config = {
        .tcp_mode = true
    };

    if (!processor_init(&processor_ctx, &processor_config, engine,
                        input_queue, output_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize processor\n");
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    // Configure output router
    output_router_config_t router_config = {
        .tcp_mode = true
    };

    if (!output_router_init(&router_ctx, &router_config, client_registry,
                            output_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize output router\n");
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    // Create threads
    pthread_t listener_tid, processor_tid, router_tid;

    if (pthread_create(&listener_tid, NULL, tcp_listener_thread, &listener_ctx) != 0) {
        fprintf(stderr, "Failed to create TCP listener thread\n");
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    if (pthread_create(&processor_tid, NULL, processor_thread, &processor_ctx) != 0) {
        fprintf(stderr, "Failed to create processor thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    if (pthread_create(&router_tid, NULL, output_router_thread, &router_ctx) != 0) {
        fprintf(stderr, "Failed to create output router thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        pthread_join(processor_tid, NULL);
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    // Wait for shutdown signal
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    // Graceful shutdown sequence
    fprintf(stderr, "\n[Main] Initiating graceful shutdown...\n");

    // 1. Stop accepting new connections (listener will stop)
    // 2. Disconnect all clients
    uint32_t disconnected_clients[MAX_TCP_CLIENTS];
    size_t num_disconnected = tcp_client_disconnect_all(client_registry,
                                                        disconnected_clients,
                                                        MAX_TCP_CLIENTS);
    fprintf(stderr, "[Main] Disconnected %zu clients\n", num_disconnected);

    // 3. Cancel all orders for disconnected clients
    for (size_t i = 0; i < num_disconnected; i++) {
        processor_cancel_client_orders(&processor_ctx, disconnected_clients[i]);
    }

    // 4. Let queues drain (give threads 2 seconds to finish)
    fprintf(stderr, "[Main] Draining queues...\n");
    sleep(2);

    // 5. Join threads
    fprintf(stderr, "[Main] Waiting for threads to finish...\n");
    pthread_join(listener_tid, NULL);
    pthread_join(processor_tid, NULL);
    pthread_join(router_tid, NULL);

    // Cleanup - free everything in reverse order
    tcp_client_registry_destroy(client_registry);
    free(client_registry);
    
    input_envelope_queue_destroy(input_queue);
    free(input_queue);
    
    output_envelope_queue_destroy(output_queue);
    free(output_queue);
    
    matching_engine_destroy(engine);
    free(engine);

    fprintf(stderr, "\n=== Matching Engine Stopped ===\n");
    return 0;
}

// UDP mode main function
static int run_udp_mode(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - UDP Mode (Legacy)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "========================================\n\n");

    // Allocate matching engine on heap
    matching_engine_t* engine = malloc(sizeof(matching_engine_t));
    if (!engine) {
        fprintf(stderr, "Failed to allocate matching engine\n");
        return 1;
    }
    matching_engine_init(engine);

    // Allocate queues on heap
    input_envelope_queue_t* input_queue = malloc(sizeof(input_envelope_queue_t));
    if (!input_queue) {
        fprintf(stderr, "Failed to allocate input queue\n");
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }
    input_envelope_queue_init(input_queue);

    output_envelope_queue_t* output_queue = malloc(sizeof(output_envelope_queue_t));
    if (!output_queue) {
        fprintf(stderr, "Failed to allocate output queue\n");
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }
    output_envelope_queue_init(output_queue);

    // Thread contexts (smaller, can stay on stack)
    udp_receiver_t receiver_ctx;
    processor_t processor_ctx;
    output_publisher_context_t publisher_ctx;

    // Initialize UDP receiver
    udp_receiver_init(&receiver_ctx, input_queue, config->port);

    // Configure processor
    processor_config_t processor_config = {
        .tcp_mode = false
    };

    if (!processor_init(&processor_ctx, &processor_config, engine,
                        input_queue, output_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize processor\n");
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    // Configure output publisher
    output_publisher_config_t publisher_config = {
        .use_binary_output = config->binary_output
    };

    if (!output_publisher_init(&publisher_ctx, &publisher_config, output_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize output publisher\n");
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    // Create threads
    pthread_t processor_tid, publisher_tid;

    // Start UDP receiver (it creates its own thread internally)
    if (!udp_receiver_start(&receiver_ctx)) {
        fprintf(stderr, "Failed to start UDP receiver\n");
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    if (pthread_create(&processor_tid, NULL, processor_thread, &processor_ctx) != 0) {
        fprintf(stderr, "Failed to create processor thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    if (pthread_create(&publisher_tid, NULL, output_publisher_thread, &publisher_ctx) != 0) {
        fprintf(stderr, "Failed to create output publisher thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        pthread_join(processor_tid, NULL);
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
        matching_engine_destroy(engine);
        free(engine);
        return 1;
    }

    // Wait for shutdown signal
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    // Graceful shutdown
    fprintf(stderr, "\n[Main] Initiating graceful shutdown...\n");
    
    // Stop UDP receiver
    udp_receiver_stop(&receiver_ctx);
    
    fprintf(stderr, "[Main] Draining queues...\n");
    sleep(2);

    fprintf(stderr, "[Main] Waiting for threads to finish...\n");
    pthread_join(processor_tid, NULL);
    pthread_join(publisher_tid, NULL);

    // Cleanup - free everything in reverse order
    udp_receiver_destroy(&receiver_ctx);
    
    input_envelope_queue_destroy(input_queue);
    free(input_queue);
    
    output_envelope_queue_destroy(output_queue);
    free(output_queue);
    
    matching_engine_destroy(engine);
    free(engine);

    fprintf(stderr, "\n=== Matching Engine Stopped ===\n");
    return 0;
}

int main(int argc, char** argv) {
    // Parse configuration
    app_config_t config;
    if (!parse_args(argc, argv, &config)) {
        return 1;
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Run in appropriate mode
    int result;
    if (config.tcp_mode) {
        result = run_tcp_mode(&config);
    } else {
        result = run_udp_mode(&config);
    }

    return result;
}

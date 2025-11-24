#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include "core/matching_engine.h"
#include "core/order_book.h"
#include "protocol/message_types_extended.h"
#include "protocol/symbol_router.h"
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
    fprintf(stderr, "  --tcp [port]        Use TCP mode (default port: 1234)\n");
    fprintf(stderr, "  --udp [port]        Use UDP mode (default port: 1234)\n");
    fprintf(stderr, "  --binary            Use binary protocol for output\n");
    fprintf(stderr, "  --dual-processor    Use dual-processor mode (A-M / N-Z) [DEFAULT]\n");
    fprintf(stderr, "  --single-processor  Use single-processor mode\n");
    fprintf(stderr, "  --help              Display this help message\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s --tcp                      # TCP, dual-processor (default)\n", program_name);
    fprintf(stderr, "  %s --tcp --single-processor   # TCP, single-processor\n", program_name);
    fprintf(stderr, "  %s --tcp 5000                 # TCP on port 5000, dual-processor\n", program_name);
    fprintf(stderr, "  %s --tcp --binary             # TCP with binary output\n", program_name);
    fprintf(stderr, "  %s --udp                      # UDP, dual-processor\n", program_name);
    fprintf(stderr, "  %s --udp --single-processor   # UDP, single-processor (legacy)\n", program_name);
    fprintf(stderr, "\nDual-Processor Mode:\n");
    fprintf(stderr, "  Symbols A-M → Processor 0 (e.g., AAPL, IBM, GOOGL, META)\n");
    fprintf(stderr, "  Symbols N-Z → Processor 1 (e.g., NVDA, TSLA, UBER, ZM)\n");
    fprintf(stderr, "  Provides ~2x throughput with parallel matching\n");
}

// Configuration
typedef struct {
    bool tcp_mode;              // true = TCP, false = UDP
    uint16_t port;              // Network port
    bool binary_output;         // true = binary, false = CSV
    bool dual_processor;        // true = dual-processor, false = single
} app_config_t;

// Parse command line arguments
static bool parse_args(int argc, char** argv, app_config_t* config) {
    // Defaults
    config->tcp_mode = true;        // Default to TCP
    config->port = 1234;
    config->binary_output = false;
    config->dual_processor = true;  // Default to dual-processor

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tcp") == 0) {
            config->tcp_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->port = (uint16_t)atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "--udp") == 0) {
            config->tcp_mode = false;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->port = (uint16_t)atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "--binary") == 0) {
            config->binary_output = true;
        } else if (strcmp(argv[i], "--dual-processor") == 0) {
            config->dual_processor = true;
        } else if (strcmp(argv[i], "--single-processor") == 0) {
            config->dual_processor = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return false;
        } else if (argv[i][0] != '-') {
            config->port = (uint16_t)atoi(argv[i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return false;
        }
    }

    return true;
}

// Print memory pool statistics
static void print_memory_stats(const char* label, const memory_pools_t* pools) {
    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, &stats);
    
    fprintf(stderr, "\n--- %s Memory Pool Statistics ---\n", label);
    fprintf(stderr, "Order Pool:\n");
    fprintf(stderr, "  Total allocations: %u\n", stats.order_allocations);
    fprintf(stderr, "  Peak usage:        %u / %u (%.1f%%)\n", 
            stats.order_peak_usage, MAX_ORDERS_IN_POOL,
            (stats.order_peak_usage * 100.0) / MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Failures:          %u\n", stats.order_failures);
    
    fprintf(stderr, "Hash Entry Pool:\n");
    fprintf(stderr, "  Total allocations: %u\n", stats.hash_allocations);
    fprintf(stderr, "  Peak usage:        %u / %u (%.1f%%)\n",
            stats.hash_peak_usage, MAX_HASH_ENTRIES_IN_POOL,
            (stats.hash_peak_usage * 100.0) / MAX_HASH_ENTRIES_IN_POOL);
    fprintf(stderr, "  Failures:          %u\n", stats.hash_failures);
}

// ============================================================================
// TCP Mode - Dual Processor
// ============================================================================
static int run_tcp_mode_dual(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - TCP Mode (Dual Processor)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "Max clients:    %d\n", MAX_TCP_CLIENTS);
    fprintf(stderr, "Processors:     2 (A-M, N-Z)\n");
    fprintf(stderr, "========================================\n");

    // ========================================================================
    // Initialize Memory Pools (one per processor for isolation)
    // ========================================================================
    memory_pools_t* pools_0 = malloc(sizeof(memory_pools_t));
    memory_pools_t* pools_1 = malloc(sizeof(memory_pools_t));
    if (!pools_0 || !pools_1) {
        fprintf(stderr, "Failed to allocate memory pools\n");
        free(pools_0);
        free(pools_1);
        return 1;
    }
    memory_pools_init(pools_0);
    memory_pools_init(pools_1);
    
    fprintf(stderr, "\nMemory Pools Initialized (per processor):\n");
    fprintf(stderr, "  Order pool:      %d slots each\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash pool:       %d slots each\n", MAX_HASH_ENTRIES_IN_POOL);
    fprintf(stderr, "  Total memory:    %.2f MB (2 x %.2f MB)\n",
            2 * ((MAX_ORDERS_IN_POOL * sizeof(order_t)) +
                 (MAX_HASH_ENTRIES_IN_POOL * sizeof(order_map_entry_t))) / (1024.0 * 1024.0),
            ((MAX_ORDERS_IN_POOL * sizeof(order_t)) +
             (MAX_HASH_ENTRIES_IN_POOL * sizeof(order_map_entry_t))) / (1024.0 * 1024.0));
    fprintf(stderr, "========================================\n\n");

    // ========================================================================
    // Initialize Matching Engines (one per processor)
    // ========================================================================
    matching_engine_t* engine_0 = malloc(sizeof(matching_engine_t));
    matching_engine_t* engine_1 = malloc(sizeof(matching_engine_t));
    if (!engine_0 || !engine_1) {
        fprintf(stderr, "Failed to allocate matching engines\n");
        free(engine_0);
        free(engine_1);
        free(pools_0);
        free(pools_1);
        return 1;
    }
    matching_engine_init(engine_0, pools_0);
    matching_engine_init(engine_1, pools_1);

    // ========================================================================
    // Initialize Client Registry (shared)
    // ========================================================================
    tcp_client_registry_t* client_registry = malloc(sizeof(tcp_client_registry_t));
    if (!client_registry) {
        fprintf(stderr, "Failed to allocate client registry\n");
        matching_engine_destroy(engine_0);
        matching_engine_destroy(engine_1);
        free(engine_0);
        free(engine_1);
        free(pools_0);
        free(pools_1);
        return 1;
    }
    tcp_client_registry_init(client_registry);

    // ========================================================================
    // Initialize Queues (2 input, 2 output)
    // ========================================================================
    input_envelope_queue_t* input_queue_0 = malloc(sizeof(input_envelope_queue_t));
    input_envelope_queue_t* input_queue_1 = malloc(sizeof(input_envelope_queue_t));
    output_envelope_queue_t* output_queue_0 = malloc(sizeof(output_envelope_queue_t));
    output_envelope_queue_t* output_queue_1 = malloc(sizeof(output_envelope_queue_t));
    
    if (!input_queue_0 || !input_queue_1 || !output_queue_0 || !output_queue_1) {
        fprintf(stderr, "Failed to allocate queues\n");
        free(input_queue_0);
        free(input_queue_1);
        free(output_queue_0);
        free(output_queue_1);
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine_0);
        matching_engine_destroy(engine_1);
        free(engine_0);
        free(engine_1);
        free(pools_0);
        free(pools_1);
        return 1;
    }
    
    input_envelope_queue_init(input_queue_0);
    input_envelope_queue_init(input_queue_1);
    output_envelope_queue_init(output_queue_0);
    output_envelope_queue_init(output_queue_1);

    // ========================================================================
    // Initialize Thread Contexts
    // ========================================================================
    tcp_listener_context_t listener_ctx;
    processor_t processor_ctx_0;
    processor_t processor_ctx_1;
    output_router_context_t router_ctx;

    // Configure TCP listener (dual mode)
    tcp_listener_config_t listener_config = {
        .port = config->port,
        .listen_backlog = 10,
        .use_binary_output = config->binary_output
    };

    if (!tcp_listener_init_dual(&listener_ctx, &listener_config, client_registry,
                                 input_queue_0, input_queue_1, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize TCP listener\n");
        goto cleanup;
    }

    // Configure processors
    processor_config_t processor_config = {
        .tcp_mode = true
    };

    if (!processor_init(&processor_ctx_0, &processor_config, engine_0,
                        input_queue_0, output_queue_0, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize processor 0\n");
        goto cleanup;
    }

    if (!processor_init(&processor_ctx_1, &processor_config, engine_1,
                        input_queue_1, output_queue_1, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize processor 1\n");
        goto cleanup;
    }

    // Configure output router (dual mode)
    output_router_config_t router_config = {
        .tcp_mode = true
    };

    if (!output_router_init_dual(&router_ctx, &router_config, client_registry,
                                  output_queue_0, output_queue_1, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize output router\n");
        goto cleanup;
    }

    // ========================================================================
    // Create Threads (4 total: listener, processor_0, processor_1, router)
    // ========================================================================
    pthread_t listener_tid, processor_0_tid, processor_1_tid, router_tid;

    if (pthread_create(&listener_tid, NULL, tcp_listener_thread, &listener_ctx) != 0) {
        fprintf(stderr, "Failed to create TCP listener thread\n");
        goto cleanup;
    }

    if (pthread_create(&processor_0_tid, NULL, processor_thread, &processor_ctx_0) != 0) {
        fprintf(stderr, "Failed to create processor 0 thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        goto cleanup;
    }

    if (pthread_create(&processor_1_tid, NULL, processor_thread, &processor_ctx_1) != 0) {
        fprintf(stderr, "Failed to create processor 1 thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        pthread_join(processor_0_tid, NULL);
        goto cleanup;
    }

    if (pthread_create(&router_tid, NULL, output_router_thread, &router_ctx) != 0) {
        fprintf(stderr, "Failed to create output router thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        pthread_join(processor_0_tid, NULL);
        pthread_join(processor_1_tid, NULL);
        goto cleanup;
    }

    fprintf(stderr, "✓ All threads started successfully (4 threads)\n");
    fprintf(stderr, "  - TCP Listener (routes by symbol)\n");
    fprintf(stderr, "  - Processor 0 (symbols A-M)\n");
    fprintf(stderr, "  - Processor 1 (symbols N-Z)\n");
    fprintf(stderr, "  - Output Router (round-robin)\n");
    fprintf(stderr, "✓ Matching engine ready\n\n");

    // Wait for shutdown signal
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    // ========================================================================
    // Graceful Shutdown
    // ========================================================================
    fprintf(stderr, "\n[Main] Initiating graceful shutdown...\n");

    // 1. Disconnect all clients
    uint32_t disconnected_clients[MAX_TCP_CLIENTS];
    size_t num_disconnected = tcp_client_disconnect_all(client_registry,
                                                        disconnected_clients,
                                                        MAX_TCP_CLIENTS);
    fprintf(stderr, "[Main] Disconnected %zu clients\n", num_disconnected);

    // 2. Cancel all orders for disconnected clients (in BOTH processors!)
    for (size_t i = 0; i < num_disconnected; i++) {
        processor_cancel_client_orders(&processor_ctx_0, disconnected_clients[i]);
        processor_cancel_client_orders(&processor_ctx_1, disconnected_clients[i]);
    }

    // 3. Let queues drain
    fprintf(stderr, "[Main] Draining queues...\n");
    sleep(2);

    // 4. Join threads
    fprintf(stderr, "[Main] Waiting for threads to finish...\n");
    pthread_join(listener_tid, NULL);
    pthread_join(processor_0_tid, NULL);
    pthread_join(processor_1_tid, NULL);
    pthread_join(router_tid, NULL);

    // Print statistics
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Final Statistics\n");
    fprintf(stderr, "========================================\n");
    
    fprintf(stderr, "\n--- Processor 0 (A-M) ---\n");
    processor_print_stats(&processor_ctx_0);
    print_memory_stats("Processor 0 (A-M)", pools_0);
    
    fprintf(stderr, "\n--- Processor 1 (N-Z) ---\n");
    processor_print_stats(&processor_ctx_1);
    print_memory_stats("Processor 1 (N-Z)", pools_1);

cleanup:
    // Cleanup in reverse order
    tcp_client_registry_destroy(client_registry);
    free(client_registry);
    
    input_envelope_queue_destroy(input_queue_0);
    input_envelope_queue_destroy(input_queue_1);
    output_envelope_queue_destroy(output_queue_0);
    output_envelope_queue_destroy(output_queue_1);
    free(input_queue_0);
    free(input_queue_1);
    free(output_queue_0);
    free(output_queue_1);
    
    matching_engine_destroy(engine_0);
    matching_engine_destroy(engine_1);
    free(engine_0);
    free(engine_1);
    
    free(pools_0);
    free(pools_1);

    fprintf(stderr, "\n=== Matching Engine Stopped ===\n");
    return 0;
}

// ============================================================================
// TCP Mode - Single Processor (backward compatible)
// ============================================================================
static int run_tcp_mode_single(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - TCP Mode (Single Processor)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "Max clients:    %d\n", MAX_TCP_CLIENTS);
    fprintf(stderr, "========================================\n");

    // Initialize memory pools
    memory_pools_t* pools = malloc(sizeof(memory_pools_t));
    if (!pools) {
        fprintf(stderr, "Failed to allocate memory pools\n");
        return 1;
    }
    memory_pools_init(pools);
    
    fprintf(stderr, "\nMemory Pools Initialized:\n");
    fprintf(stderr, "  Order pool:      %d slots\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash pool:       %d slots\n", MAX_HASH_ENTRIES_IN_POOL);
    fprintf(stderr, "  Total memory:    %.2f MB\n",
            ((MAX_ORDERS_IN_POOL * sizeof(order_t)) +
             (MAX_HASH_ENTRIES_IN_POOL * sizeof(order_map_entry_t))) / (1024.0 * 1024.0));
    fprintf(stderr, "========================================\n\n");

    // Initialize matching engine
    matching_engine_t* engine = malloc(sizeof(matching_engine_t));
    if (!engine) {
        fprintf(stderr, "Failed to allocate matching engine\n");
        free(pools);
        return 1;
    }
    matching_engine_init(engine, pools);

    // Initialize client registry
    tcp_client_registry_t* client_registry = malloc(sizeof(tcp_client_registry_t));
    if (!client_registry) {
        fprintf(stderr, "Failed to allocate client registry\n");
        matching_engine_destroy(engine);
        free(engine);
        free(pools);
        return 1;
    }
    tcp_client_registry_init(client_registry);

    // Initialize queues
    input_envelope_queue_t* input_queue = malloc(sizeof(input_envelope_queue_t));
    output_envelope_queue_t* output_queue = malloc(sizeof(output_envelope_queue_t));
    if (!input_queue || !output_queue) {
        fprintf(stderr, "Failed to allocate queues\n");
        free(input_queue);
        free(output_queue);
        tcp_client_registry_destroy(client_registry);
        free(client_registry);
        matching_engine_destroy(engine);
        free(engine);
        free(pools);
        return 1;
    }
    input_envelope_queue_init(input_queue);
    output_envelope_queue_init(output_queue);

    // Thread contexts
    tcp_listener_context_t listener_ctx;
    processor_t processor_ctx;
    output_router_context_t router_ctx;

    // Configure TCP listener (single mode)
    tcp_listener_config_t listener_config = {
        .port = config->port,
        .listen_backlog = 10,
        .use_binary_output = config->binary_output
    };

    if (!tcp_listener_init(&listener_ctx, &listener_config, client_registry,
                           input_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize TCP listener\n");
        goto cleanup;
    }

    // Configure processor
    processor_config_t processor_config = {
        .tcp_mode = true
    };

    if (!processor_init(&processor_ctx, &processor_config, engine,
                        input_queue, output_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize processor\n");
        goto cleanup;
    }

    // Configure output router (single mode)
    output_router_config_t router_config = {
        .tcp_mode = true
    };

    if (!output_router_init(&router_ctx, &router_config, client_registry,
                            output_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize output router\n");
        goto cleanup;
    }

    // Create threads
    pthread_t listener_tid, processor_tid, router_tid;

    if (pthread_create(&listener_tid, NULL, tcp_listener_thread, &listener_ctx) != 0) {
        fprintf(stderr, "Failed to create TCP listener thread\n");
        goto cleanup;
    }

    if (pthread_create(&processor_tid, NULL, processor_thread, &processor_ctx) != 0) {
        fprintf(stderr, "Failed to create processor thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        goto cleanup;
    }

    if (pthread_create(&router_tid, NULL, output_router_thread, &router_ctx) != 0) {
        fprintf(stderr, "Failed to create output router thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        pthread_join(processor_tid, NULL);
        goto cleanup;
    }

    fprintf(stderr, "✓ All threads started successfully\n");
    fprintf(stderr, "✓ Matching engine ready\n\n");

    // Wait for shutdown signal
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    // Graceful shutdown
    fprintf(stderr, "\n[Main] Initiating graceful shutdown...\n");

    uint32_t disconnected_clients[MAX_TCP_CLIENTS];
    size_t num_disconnected = tcp_client_disconnect_all(client_registry,
                                                        disconnected_clients,
                                                        MAX_TCP_CLIENTS);
    fprintf(stderr, "[Main] Disconnected %zu clients\n", num_disconnected);

    for (size_t i = 0; i < num_disconnected; i++) {
        processor_cancel_client_orders(&processor_ctx, disconnected_clients[i]);
    }

    fprintf(stderr, "[Main] Draining queues...\n");
    sleep(2);

    fprintf(stderr, "[Main] Waiting for threads to finish...\n");
    pthread_join(listener_tid, NULL);
    pthread_join(processor_tid, NULL);
    pthread_join(router_tid, NULL);

    print_memory_stats("Single Processor", pools);

cleanup:
    tcp_client_registry_destroy(client_registry);
    free(client_registry);
    
    input_envelope_queue_destroy(input_queue);
    output_envelope_queue_destroy(output_queue);
    free(input_queue);
    free(output_queue);
    
    matching_engine_destroy(engine);
    free(engine);
    
    free(pools);

    fprintf(stderr, "\n=== Matching Engine Stopped ===\n");
    return 0;
}

// ============================================================================
// UDP Mode - Dual Processor
// ============================================================================
static int run_udp_mode_dual(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - UDP Mode (Dual Processor)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "Processors:     2 (A-M, N-Z)\n");
    fprintf(stderr, "========================================\n");

    // Initialize memory pools (one per processor)
    memory_pools_t* pools_0 = malloc(sizeof(memory_pools_t));
    memory_pools_t* pools_1 = malloc(sizeof(memory_pools_t));
    if (!pools_0 || !pools_1) {
        fprintf(stderr, "Failed to allocate memory pools\n");
        free(pools_0);
        free(pools_1);
        return 1;
    }
    memory_pools_init(pools_0);
    memory_pools_init(pools_1);
    
    fprintf(stderr, "\nMemory Pools Initialized (per processor):\n");
    fprintf(stderr, "  Order pool:      %d slots each\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash pool:       %d slots each\n", MAX_HASH_ENTRIES_IN_POOL);
    fprintf(stderr, "========================================\n\n");

    // Initialize matching engines
    matching_engine_t* engine_0 = malloc(sizeof(matching_engine_t));
    matching_engine_t* engine_1 = malloc(sizeof(matching_engine_t));
    if (!engine_0 || !engine_1) {
        fprintf(stderr, "Failed to allocate matching engines\n");
        free(engine_0);
        free(engine_1);
        free(pools_0);
        free(pools_1);
        return 1;
    }
    matching_engine_init(engine_0, pools_0);
    matching_engine_init(engine_1, pools_1);

    // Initialize queues
    input_envelope_queue_t* input_queue_0 = malloc(sizeof(input_envelope_queue_t));
    input_envelope_queue_t* input_queue_1 = malloc(sizeof(input_envelope_queue_t));
    output_envelope_queue_t* output_queue_0 = malloc(sizeof(output_envelope_queue_t));
    output_envelope_queue_t* output_queue_1 = malloc(sizeof(output_envelope_queue_t));
    
    if (!input_queue_0 || !input_queue_1 || !output_queue_0 || !output_queue_1) {
        fprintf(stderr, "Failed to allocate queues\n");
        free(input_queue_0);
        free(input_queue_1);
        free(output_queue_0);
        free(output_queue_1);
        matching_engine_destroy(engine_0);
        matching_engine_destroy(engine_1);
        free(engine_0);
        free(engine_1);
        free(pools_0);
        free(pools_1);
        return 1;
    }
    
    input_envelope_queue_init(input_queue_0);
    input_envelope_queue_init(input_queue_1);
    output_envelope_queue_init(output_queue_0);
    output_envelope_queue_init(output_queue_1);

    // Thread contexts
    udp_receiver_t receiver_ctx;
    processor_t processor_ctx_0;
    processor_t processor_ctx_1;
    output_publisher_context_t publisher_ctx;

    // Initialize UDP receiver (dual mode)
    udp_receiver_init_dual(&receiver_ctx, input_queue_0, input_queue_1, config->port);

    // Configure processors
    processor_config_t processor_config = {
        .tcp_mode = false
    };

    if (!processor_init(&processor_ctx_0, &processor_config, engine_0,
                        input_queue_0, output_queue_0, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize processor 0\n");
        goto cleanup;
    }

    if (!processor_init(&processor_ctx_1, &processor_config, engine_1,
                        input_queue_1, output_queue_1, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize processor 1\n");
        goto cleanup;
    }

    // Configure output publisher
    // NOTE: For UDP dual-processor, we need a merged output
    // For simplicity, we'll use processor 0's output for now
    // A proper implementation would merge both output queues
    output_publisher_config_t publisher_config = {
        .use_binary_output = config->binary_output
    };

    if (!output_publisher_init(&publisher_ctx, &publisher_config, output_queue_0, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize output publisher\n");
        goto cleanup;
    }

    // TODO: For proper UDP dual-processor support, implement a merging output publisher
    // that reads from both output_queue_0 and output_queue_1
    fprintf(stderr, "WARNING: UDP dual-processor mode outputs only processor 0 results to stdout\n");
    fprintf(stderr, "         Use TCP mode for full dual-processor output support\n\n");

    // Create threads
    pthread_t processor_0_tid, processor_1_tid, publisher_tid;

    // Start UDP receiver
    if (!udp_receiver_start(&receiver_ctx)) {
        fprintf(stderr, "Failed to start UDP receiver\n");
        goto cleanup;
    }

    if (pthread_create(&processor_0_tid, NULL, processor_thread, &processor_ctx_0) != 0) {
        fprintf(stderr, "Failed to create processor 0 thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        goto cleanup;
    }

    if (pthread_create(&processor_1_tid, NULL, processor_thread, &processor_ctx_1) != 0) {
        fprintf(stderr, "Failed to create processor 1 thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        pthread_join(processor_0_tid, NULL);
        goto cleanup;
    }

    if (pthread_create(&publisher_tid, NULL, output_publisher_thread, &publisher_ctx) != 0) {
        fprintf(stderr, "Failed to create output publisher thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        pthread_join(processor_0_tid, NULL);
        pthread_join(processor_1_tid, NULL);
        goto cleanup;
    }

    fprintf(stderr, "✓ All threads started successfully (4 threads)\n");
    fprintf(stderr, "  - UDP Receiver (routes by symbol)\n");
    fprintf(stderr, "  - Processor 0 (symbols A-M)\n");
    fprintf(stderr, "  - Processor 1 (symbols N-Z)\n");
    fprintf(stderr, "  - Output Publisher\n");
    fprintf(stderr, "✓ Matching engine ready\n\n");

    // Wait for shutdown signal
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    // Graceful shutdown
    fprintf(stderr, "\n[Main] Initiating graceful shutdown...\n");
    
    udp_receiver_stop(&receiver_ctx);
    
    fprintf(stderr, "[Main] Draining queues...\n");
    sleep(2);

    fprintf(stderr, "[Main] Waiting for threads to finish...\n");
    pthread_join(processor_0_tid, NULL);
    pthread_join(processor_1_tid, NULL);
    pthread_join(publisher_tid, NULL);

    // Print statistics
    fprintf(stderr, "\n--- Processor 0 (A-M) ---\n");
    processor_print_stats(&processor_ctx_0);
    print_memory_stats("Processor 0 (A-M)", pools_0);
    
    fprintf(stderr, "\n--- Processor 1 (N-Z) ---\n");
    processor_print_stats(&processor_ctx_1);
    print_memory_stats("Processor 1 (N-Z)", pools_1);

cleanup:
    udp_receiver_destroy(&receiver_ctx);
    
    input_envelope_queue_destroy(input_queue_0);
    input_envelope_queue_destroy(input_queue_1);
    output_envelope_queue_destroy(output_queue_0);
    output_envelope_queue_destroy(output_queue_1);
    free(input_queue_0);
    free(input_queue_1);
    free(output_queue_0);
    free(output_queue_1);
    
    matching_engine_destroy(engine_0);
    matching_engine_destroy(engine_1);
    free(engine_0);
    free(engine_1);
    
    free(pools_0);
    free(pools_1);

    fprintf(stderr, "\n=== Matching Engine Stopped ===\n");
    return 0;
}

// ============================================================================
// UDP Mode - Single Processor (backward compatible / legacy)
// ============================================================================
static int run_udp_mode_single(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - UDP Mode (Single Processor)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "========================================\n");

    // Initialize memory pools
    memory_pools_t* pools = malloc(sizeof(memory_pools_t));
    if (!pools) {
        fprintf(stderr, "Failed to allocate memory pools\n");
        return 1;
    }
    memory_pools_init(pools);
    
    fprintf(stderr, "\nMemory Pools Initialized:\n");
    fprintf(stderr, "  Order pool:      %d slots\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash pool:       %d slots\n", MAX_HASH_ENTRIES_IN_POOL);
    fprintf(stderr, "========================================\n\n");

    // Initialize matching engine
    matching_engine_t* engine = malloc(sizeof(matching_engine_t));
    if (!engine) {
        fprintf(stderr, "Failed to allocate matching engine\n");
        free(pools);
        return 1;
    }
    matching_engine_init(engine, pools);

    // Initialize queues
    input_envelope_queue_t* input_queue = malloc(sizeof(input_envelope_queue_t));
    output_envelope_queue_t* output_queue = malloc(sizeof(output_envelope_queue_t));
    if (!input_queue || !output_queue) {
        fprintf(stderr, "Failed to allocate queues\n");
        free(input_queue);
        free(output_queue);
        matching_engine_destroy(engine);
        free(engine);
        free(pools);
        return 1;
    }
    input_envelope_queue_init(input_queue);
    output_envelope_queue_init(output_queue);

    // Thread contexts
    udp_receiver_t receiver_ctx;
    processor_t processor_ctx;
    output_publisher_context_t publisher_ctx;

    // Initialize UDP receiver (single mode)
    udp_receiver_init(&receiver_ctx, input_queue, config->port);

    // Configure processor
    processor_config_t processor_config = {
        .tcp_mode = false
    };

    if (!processor_init(&processor_ctx, &processor_config, engine,
                        input_queue, output_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize processor\n");
        goto cleanup;
    }

    // Configure output publisher
    output_publisher_config_t publisher_config = {
        .use_binary_output = config->binary_output
    };

    if (!output_publisher_init(&publisher_ctx, &publisher_config, output_queue, &g_shutdown)) {
        fprintf(stderr, "Failed to initialize output publisher\n");
        goto cleanup;
    }

    // Create threads
    pthread_t processor_tid, publisher_tid;

    // Start UDP receiver
    if (!udp_receiver_start(&receiver_ctx)) {
        fprintf(stderr, "Failed to start UDP receiver\n");
        goto cleanup;
    }

    if (pthread_create(&processor_tid, NULL, processor_thread, &processor_ctx) != 0) {
        fprintf(stderr, "Failed to create processor thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        goto cleanup;
    }

    if (pthread_create(&publisher_tid, NULL, output_publisher_thread, &publisher_ctx) != 0) {
        fprintf(stderr, "Failed to create output publisher thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        pthread_join(processor_tid, NULL);
        goto cleanup;
    }

    fprintf(stderr, "✓ All threads started successfully\n");
    fprintf(stderr, "✓ Matching engine ready\n\n");

    // Wait for shutdown signal
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    // Graceful shutdown
    fprintf(stderr, "\n[Main] Initiating graceful shutdown...\n");
    
    udp_receiver_stop(&receiver_ctx);
    
    fprintf(stderr, "[Main] Draining queues...\n");
    sleep(2);

    fprintf(stderr, "[Main] Waiting for threads to finish...\n");
    pthread_join(processor_tid, NULL);
    pthread_join(publisher_tid, NULL);

    print_memory_stats("Single Processor", pools);

cleanup:
    udp_receiver_destroy(&receiver_ctx);
    
    input_envelope_queue_destroy(input_queue);
    output_envelope_queue_destroy(output_queue);
    free(input_queue);
    free(output_queue);
    
    matching_engine_destroy(engine);
    free(engine);
    
    free(pools);

    fprintf(stderr, "\n=== Matching Engine Stopped ===\n");
    return 0;
}

// ============================================================================
// Main Entry Point
// ============================================================================
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
        if (config.dual_processor) {
            result = run_tcp_mode_dual(&config);
        } else {
            result = run_tcp_mode_single(&config);
        }
    } else {
        if (config.dual_processor) {
            result = run_udp_mode_dual(&config);
        } else {
            result = run_udp_mode_single(&config);
        }
    }

    return result;
}

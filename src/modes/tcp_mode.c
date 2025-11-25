#include "modes/tcp_mode.h"
#include "modes/multicast_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include "core/matching_engine.h"
#include "core/order_book.h"
#include "network/tcp_listener.h"
#include "network/tcp_connection.h"
#include "threading/processor.h"
#include "threading/output_router.h"

// External shutdown flag (from main.c)
extern atomic_bool g_shutdown;

/**
 * TCP Mode - Dual Processor Implementation
 */
int run_tcp_dual_processor(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - TCP Mode (Dual Processor)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "Max clients:    %d\n", MAX_TCP_CLIENTS);
    fprintf(stderr, "Processors:     2 (A-M, N-Z)\n");
    if (multicast_is_enabled(config)) {
        fprintf(stderr, "Multicast:      %s:%u (✓ ENABLED)\n", 
                config->multicast_group, config->multicast_port);
        fprintf(stderr, "Threads:        5 (Listener, Proc0, Proc1, Router, Multicast)\n");
    } else {
        fprintf(stderr, "Multicast:      Disabled\n");
        fprintf(stderr, "Threads:        4 (Listener, Proc0, Proc1, Router)\n");
    }
    fprintf(stderr, "========================================\n");

    // ========================================================================
    // Initialize Memory Pools (one per processor for isolation)
    // ========================================================================
    memory_pools_t* pools_0 = malloc(sizeof(memory_pools_t));
    memory_pools_t* pools_1 = malloc(sizeof(memory_pools_t));
    if (!pools_0 || !pools_1) {
        fprintf(stderr, "[TCP Dual] Failed to allocate memory pools\n");
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
        fprintf(stderr, "[TCP Dual] Failed to allocate matching engines\n");
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
        fprintf(stderr, "[TCP Dual] Failed to allocate client registry\n");
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
        fprintf(stderr, "[TCP Dual] Failed to allocate queues\n");
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
    multicast_helper_t mcast_helper;

    // Configure TCP listener (dual mode)
    tcp_listener_config_t listener_config = {
        .port = config->port,
        .listen_backlog = 10,
        .use_binary_output = config->binary_output
    };

    if (!tcp_listener_init_dual(&listener_ctx, &listener_config, client_registry,
                                 input_queue_0, input_queue_1, &g_shutdown)) {
        fprintf(stderr, "[TCP Dual] Failed to initialize TCP listener\n");
        goto cleanup;
    }

    // Configure processors
    processor_config_t processor_config = {
        .tcp_mode = true
    };

    if (!processor_init(&processor_ctx_0, &processor_config, engine_0,
                        input_queue_0, output_queue_0, &g_shutdown)) {
        fprintf(stderr, "[TCP Dual] Failed to initialize processor 0\n");
        goto cleanup;
    }

    if (!processor_init(&processor_ctx_1, &processor_config, engine_1,
                        input_queue_1, output_queue_1, &g_shutdown)) {
        fprintf(stderr, "[TCP Dual] Failed to initialize processor 1\n");
        goto cleanup;
    }

    // Configure output router (dual mode)
    output_router_config_t router_config = {
        .tcp_mode = true
    };

    if (!output_router_init_dual(&router_ctx, &router_config, client_registry,
                                  output_queue_0, output_queue_1, &g_shutdown)) {
        fprintf(stderr, "[TCP Dual] Failed to initialize output router\n");
        goto cleanup;
    }

    // Configure multicast publisher (OPTIONAL - separate module!)
    if (multicast_is_enabled(config)) {
        if (!multicast_setup_dual(&mcast_helper, config, output_queue_0, output_queue_1, &g_shutdown)) {
            fprintf(stderr, "[TCP Dual] Failed to setup multicast\n");
            goto cleanup;
        }
    }

    // ========================================================================
    // Create Threads (4 core + 1 optional multicast)
    // ========================================================================
    pthread_t listener_tid, processor_0_tid, processor_1_tid, router_tid;

    if (pthread_create(&listener_tid, NULL, tcp_listener_thread, &listener_ctx) != 0) {
        fprintf(stderr, "[TCP Dual] Failed to create TCP listener thread\n");
        goto cleanup;
    }

    if (pthread_create(&processor_0_tid, NULL, processor_thread, &processor_ctx_0) != 0) {
        fprintf(stderr, "[TCP Dual] Failed to create processor 0 thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        goto cleanup;
    }

    if (pthread_create(&processor_1_tid, NULL, processor_thread, &processor_ctx_1) != 0) {
        fprintf(stderr, "[TCP Dual] Failed to create processor 1 thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        pthread_join(processor_0_tid, NULL);
        goto cleanup;
    }

    if (pthread_create(&router_tid, NULL, output_router_thread, &router_ctx) != 0) {
        fprintf(stderr, "[TCP Dual] Failed to create output router thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        pthread_join(processor_0_tid, NULL);
        pthread_join(processor_1_tid, NULL);
        goto cleanup;
    }

    // Start multicast thread (OPTIONAL - 5th thread!)
    if (multicast_is_enabled(config)) {
        if (!multicast_start(&mcast_helper)) {
            fprintf(stderr, "[TCP Dual] Failed to start multicast thread\n");
            atomic_store(&g_shutdown, true);
            pthread_join(listener_tid, NULL);
            pthread_join(processor_0_tid, NULL);
            pthread_join(processor_1_tid, NULL);
            pthread_join(router_tid, NULL);
            goto cleanup;
        }
        
        fprintf(stderr, "✓ All threads started successfully (5 threads)\n");
        fprintf(stderr, "  - TCP Listener (routes by symbol)\n");
        fprintf(stderr, "  - Processor 0 (symbols A-M)\n");
        fprintf(stderr, "  - Processor 1 (symbols N-Z)\n");
        fprintf(stderr, "  - Output Router (round-robin → TCP clients)\n");
        fprintf(stderr, "  - Multicast Publisher (broadcasts to %s:%u)\n",
                config->multicast_group, config->multicast_port);
    } else {
        fprintf(stderr, "✓ All threads started successfully (4 threads)\n");
        fprintf(stderr, "  - TCP Listener (routes by symbol)\n");
        fprintf(stderr, "  - Processor 0 (symbols A-M)\n");
        fprintf(stderr, "  - Processor 1 (symbols N-Z)\n");
        fprintf(stderr, "  - Output Router (round-robin)\n");
    }
    
    fprintf(stderr, "✓ Matching engine ready\n\n");
    
    if (multicast_is_enabled(config)) {
        fprintf(stderr, "💡 Multicast subscribers can connect with:\n");
        fprintf(stderr, "   ./multicast_subscriber %s %u\n\n",
                config->multicast_group, config->multicast_port);
    }

    // Wait for shutdown signal
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    // ========================================================================
    // Graceful Shutdown
    // ========================================================================
    fprintf(stderr, "\n[TCP Dual] Initiating graceful shutdown...\n");

    // 1. Disconnect all clients
    uint32_t disconnected_clients[MAX_TCP_CLIENTS];
    size_t num_disconnected = tcp_client_disconnect_all(client_registry,
                                                        disconnected_clients,
                                                        MAX_TCP_CLIENTS);
    fprintf(stderr, "[TCP Dual] Disconnected %zu clients\n", num_disconnected);

    // 2. Cancel all orders for disconnected clients (in BOTH processors!)
    for (size_t i = 0; i < num_disconnected; i++) {
        processor_cancel_client_orders(&processor_ctx_0, disconnected_clients[i]);
        processor_cancel_client_orders(&processor_ctx_1, disconnected_clients[i]);
    }

    // 3. Let queues drain
    fprintf(stderr, "[TCP Dual] Draining queues...\n");
    sleep(2);

    // 4. Join threads
    fprintf(stderr, "[TCP Dual] Waiting for threads to finish...\n");
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
    // Cleanup multicast (if enabled)
    if (multicast_is_enabled(config)) {
        multicast_cleanup(&mcast_helper);
    }
    
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

    fprintf(stderr, "\n=== TCP Dual Processor Mode Stopped ===\n");
    return 0;
}

/**
 * TCP Mode - Single Processor Implementation
 */
int run_tcp_single_processor(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - TCP Mode (Single Processor)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "Max clients:    %d\n", MAX_TCP_CLIENTS);
    if (multicast_is_enabled(config)) {
        fprintf(stderr, "Multicast:      %s:%u (✓ ENABLED)\n",
                config->multicast_group, config->multicast_port);
        fprintf(stderr, "Threads:        4 (Listener, Processor, Router, Multicast)\n");
    } else {
        fprintf(stderr, "Threads:        3 (Listener, Processor, Router)\n");
    }
    fprintf(stderr, "========================================\n");

    // Initialize memory pools
    memory_pools_t* pools = malloc(sizeof(memory_pools_t));
    if (!pools) {
        fprintf(stderr, "[TCP Single] Failed to allocate memory pools\n");
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
        fprintf(stderr, "[TCP Single] Failed to allocate matching engine\n");
        free(pools);
        return 1;
    }
    matching_engine_init(engine, pools);

    // Initialize client registry
    tcp_client_registry_t* client_registry = malloc(sizeof(tcp_client_registry_t));
    if (!client_registry) {
        fprintf(stderr, "[TCP Single] Failed to allocate client registry\n");
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
        fprintf(stderr, "[TCP Single] Failed to allocate queues\n");
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
    multicast_helper_t mcast_helper;

    // Configure TCP listener (single mode)
    tcp_listener_config_t listener_config = {
        .port = config->port,
        .listen_backlog = 10,
        .use_binary_output = config->binary_output
    };

    if (!tcp_listener_init(&listener_ctx, &listener_config, client_registry,
                           input_queue, &g_shutdown)) {
        fprintf(stderr, "[TCP Single] Failed to initialize TCP listener\n");
        goto cleanup;
    }

    // Configure processor
    processor_config_t processor_config = {
        .tcp_mode = true
    };

    if (!processor_init(&processor_ctx, &processor_config, engine,
                        input_queue, output_queue, &g_shutdown)) {
        fprintf(stderr, "[TCP Single] Failed to initialize processor\n");
        goto cleanup;
    }

    // Configure output router (single mode)
    output_router_config_t router_config = {
        .tcp_mode = true
    };

    if (!output_router_init(&router_ctx, &router_config, client_registry,
                            output_queue, &g_shutdown)) {
        fprintf(stderr, "[TCP Single] Failed to initialize output router\n");
        goto cleanup;
    }

    // Configure multicast publisher (OPTIONAL)
    if (multicast_is_enabled(config)) {
        if (!multicast_setup_single(&mcast_helper, config, output_queue, &g_shutdown)) {
            fprintf(stderr, "[TCP Single] Failed to setup multicast\n");
            goto cleanup;
        }
    }

    // Create threads
    pthread_t listener_tid, processor_tid, router_tid;

    if (pthread_create(&listener_tid, NULL, tcp_listener_thread, &listener_ctx) != 0) {
        fprintf(stderr, "[TCP Single] Failed to create TCP listener thread\n");
        goto cleanup;
    }

    if (pthread_create(&processor_tid, NULL, processor_thread, &processor_ctx) != 0) {
        fprintf(stderr, "[TCP Single] Failed to create processor thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        goto cleanup;
    }

    if (pthread_create(&router_tid, NULL, output_router_thread, &router_ctx) != 0) {
        fprintf(stderr, "[TCP Single] Failed to create output router thread\n");
        atomic_store(&g_shutdown, true);
        pthread_join(listener_tid, NULL);
        pthread_join(processor_tid, NULL);
        goto cleanup;
    }

    if (multicast_is_enabled(config)) {
        if (!multicast_start(&mcast_helper)) {
            fprintf(stderr, "[TCP Single] Failed to start multicast thread\n");
            atomic_store(&g_shutdown, true);
            pthread_join(listener_tid, NULL);
            pthread_join(processor_tid, NULL);
            pthread_join(router_tid, NULL);
            goto cleanup;
        }
    }

    fprintf(stderr, "✓ All threads started successfully\n");
    fprintf(stderr, "✓ Matching engine ready\n\n");

    if (multicast_is_enabled(config)) {
        fprintf(stderr, "💡 Multicast subscribers can connect with:\n");
        fprintf(stderr, "   ./multicast_subscriber %s %u\n\n",
                config->multicast_group, config->multicast_port);
    }

    // Wait for shutdown signal
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    // Graceful shutdown
    fprintf(stderr, "\n[TCP Single] Initiating graceful shutdown...\n");

    uint32_t disconnected_clients[MAX_TCP_CLIENTS];
    size_t num_disconnected = tcp_client_disconnect_all(client_registry,
                                                        disconnected_clients,
                                                        MAX_TCP_CLIENTS);
    fprintf(stderr, "[TCP Single] Disconnected %zu clients\n", num_disconnected);

    for (size_t i = 0; i < num_disconnected; i++) {
        processor_cancel_client_orders(&processor_ctx, disconnected_clients[i]);
    }

    fprintf(stderr, "[TCP Single] Draining queues...\n");
    sleep(2);

    fprintf(stderr, "[TCP Single] Waiting for threads to finish...\n");
    pthread_join(listener_tid, NULL);
    pthread_join(processor_tid, NULL);
    pthread_join(router_tid, NULL);

    print_memory_stats("Single Processor", pools);

cleanup:
    if (multicast_is_enabled(config)) {
        multicast_cleanup(&mcast_helper);
    }
    
    tcp_client_registry_destroy(client_registry);
    free(client_registry);
    
    input_envelope_queue_destroy(input_queue);
    output_envelope_queue_destroy(output_queue);
    free(input_queue);
    free(output_queue);
    
    matching_engine_destroy(engine);
    free(engine);
    
    free(pools);

    fprintf(stderr, "\n=== TCP Single Processor Mode Stopped ===\n");
    return 0;
}

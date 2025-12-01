#include "modes/tcp_mode.h"
#include "modes/multicast_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdalign.h>
#include "core/matching_engine.h"
#include "core/order_book.h"
#include "network/tcp_listener.h"
#include "network/tcp_connection.h"
#include "threading/processor.h"
#include "threading/output_router.h"

extern atomic_bool g_shutdown;

/* ============================================================================
 * Aligned Allocation Helper
 * ============================================================================
 * Power of Ten Rule 3: No dynamic allocation AFTER initialization.
 * Allocation at startup is compliant - we allocate once, use throughout,
 * and free at shutdown.
 */

static inline void* aligned_alloc_64(size_t size) {
    void* ptr = NULL;
    if (posix_memalign(&ptr, 64, size) != 0) {
        return NULL;
    }
    return ptr;
}

/* ============================================================================
 * TCP Mode - Dual Processor Implementation
 * ============================================================================ */

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
    } else {
        fprintf(stderr, "Multicast:      Disabled\n");
    }
    fprintf(stderr, "Threads:        4 (Listener, Proc0, Proc1, Router)\n");
    fprintf(stderr, "========================================\n");

    int result = 0;
    
    /* ========================================================================
     * Power of Ten Rule 3 Compliant: Allocate all large structures at init
     * ======================================================================== */
    
    /* Memory pools - one per processor for isolation */
    memory_pools_t* pools_0 = aligned_alloc_64(sizeof(memory_pools_t));
    memory_pools_t* pools_1 = aligned_alloc_64(sizeof(memory_pools_t));
    
    /* Matching engines - one per processor */
    matching_engine_t* engine_0 = aligned_alloc_64(sizeof(matching_engine_t));
    matching_engine_t* engine_1 = aligned_alloc_64(sizeof(matching_engine_t));
    
    /* Shared client registry */
    tcp_client_registry_t* client_registry = aligned_alloc_64(sizeof(tcp_client_registry_t));
    
    /* Lock-free queues - 64-byte aligned for cache line optimization */
    input_envelope_queue_t* input_queue_0 = aligned_alloc_64(sizeof(input_envelope_queue_t));
    input_envelope_queue_t* input_queue_1 = aligned_alloc_64(sizeof(input_envelope_queue_t));
    output_envelope_queue_t* output_queue_0 = aligned_alloc_64(sizeof(output_envelope_queue_t));
    output_envelope_queue_t* output_queue_1 = aligned_alloc_64(sizeof(output_envelope_queue_t));
    
    /* Rule 7: Check all allocations */
    if (!pools_0 || !pools_1 || !engine_0 || !engine_1 || !client_registry ||
        !input_queue_0 || !input_queue_1 || !output_queue_0 || !output_queue_1) {
        fprintf(stderr, "[TCP Dual] FATAL: Failed to allocate core data structures\n");
        fprintf(stderr, "  pools_0=%p pools_1=%p engine_0=%p engine_1=%p\n",
                (void*)pools_0, (void*)pools_1, (void*)engine_0, (void*)engine_1);
        fprintf(stderr, "  client_registry=%p\n", (void*)client_registry);
        fprintf(stderr, "  input_queue_0=%p input_queue_1=%p\n",
                (void*)input_queue_0, (void*)input_queue_1);
        fprintf(stderr, "  output_queue_0=%p output_queue_1=%p\n",
                (void*)output_queue_0, (void*)output_queue_1);
        result = -1;
        goto cleanup_alloc;
    }
    
    fprintf(stderr, "\n✓ Core structures allocated (heap, 64-byte aligned)\n");

    /* Initialize memory pools */
    memory_pools_init(pools_0);
    memory_pools_init(pools_1);
    
    fprintf(stderr, "\nMemory Pools Initialized (per processor):\n");
    fprintf(stderr, "  Order pool:      %d slots each\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash table:      %d slots (open-addressing)\n", ORDER_MAP_SIZE);
    fprintf(stderr, "  Total memory:    ~%.2f MB per processor\n",
        ((MAX_ORDERS_IN_POOL * sizeof(order_t)) + 
         (ORDER_MAP_SIZE * sizeof(order_map_slot_t))) / (1024.0 * 1024.0));
    fprintf(stderr, "========================================\n\n");

    /* Initialize matching engines */
    matching_engine_init(engine_0, pools_0);
    matching_engine_init(engine_1, pools_1);

    /* Initialize client registry */
    tcp_client_registry_init(client_registry);

    /* Initialize queues */
    input_envelope_queue_init(input_queue_0);
    input_envelope_queue_init(input_queue_1);
    output_envelope_queue_init(output_queue_0);
    output_envelope_queue_init(output_queue_1);

    /* Thread contexts (stack allocated - small structs) */
    tcp_listener_context_t listener_ctx;
    processor_t processor_ctx_0;
    processor_t processor_ctx_1;
    output_router_context_t router_ctx;
    
    /* Zero-initialize contexts */
    memset(&listener_ctx, 0, sizeof(listener_ctx));
    memset(&processor_ctx_0, 0, sizeof(processor_ctx_0));
    memset(&processor_ctx_1, 0, sizeof(processor_ctx_1));
    memset(&router_ctx, 0, sizeof(router_ctx));

    /* Configure TCP listener (dual mode) */
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

    /* Configure processors */
    processor_config_t processor_config_0 = {
        .processor_id = 0,
        .tcp_mode = true,
        .spin_wait = false
    };
    processor_config_t processor_config_1 = {
        .processor_id = 1,
        .tcp_mode = true,
        .spin_wait = false
    };

    if (!processor_init(&processor_ctx_0, &processor_config_0, engine_0,
                        input_queue_0, output_queue_0, &g_shutdown)) {
        fprintf(stderr, "[TCP Dual] Failed to initialize processor 0\n");
        goto cleanup;
    }

    if (!processor_init(&processor_ctx_1, &processor_config_1, engine_1,
                        input_queue_1, output_queue_1, &g_shutdown)) {
        fprintf(stderr, "[TCP Dual] Failed to initialize processor 1\n");
        goto cleanup;
    }

    /* Configure output router (dual mode) */
    output_router_config_t router_config = {
        .tcp_mode = true
    };

    if (!output_router_init_dual(&router_ctx, &router_config, client_registry,
                                  output_queue_0, output_queue_1, &g_shutdown)) {
        fprintf(stderr, "[TCP Dual] Failed to initialize output router\n");
        goto cleanup;
    }

    /* Enable multicast on the output router (if configured) */
    if (multicast_is_enabled(config)) {
        if (!output_router_enable_multicast(&router_ctx,
                                            config->multicast_group,
                                            config->multicast_port,
                                            1,  /* TTL = 1 for local subnet */
                                            config->binary_output)) {
            fprintf(stderr, "[TCP Dual] Failed to enable multicast on router\n");
            goto cleanup;
        }
    }

    /* Create threads */
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

    fprintf(stderr, "✓ All threads started successfully (4 threads)\n");
    fprintf(stderr, "  - TCP Listener (routes by symbol)\n");
    fprintf(stderr, "  - Processor 0 (symbols A-M)\n");
    fprintf(stderr, "  - Processor 1 (symbols N-Z)\n");
    fprintf(stderr, "  - Output Router (TCP unicast%s)\n",
            multicast_is_enabled(config) ? " + multicast broadcast" : "");
    
    fprintf(stderr, "✓ Matching engine ready\n\n");
    
    if (multicast_is_enabled(config)) {
        fprintf(stderr, "💡 Multicast subscribers can connect with:\n");
        fprintf(stderr, "   ./multicast_subscriber %s %u\n\n",
                config->multicast_group, config->multicast_port);
    }

    /* Wait for shutdown signal */
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    /* Graceful shutdown */
    fprintf(stderr, "\n[TCP Dual] Initiating graceful shutdown...\n");

    /* Disconnect all clients */
    uint32_t disconnected_clients[MAX_TCP_CLIENTS];
    size_t num_disconnected = tcp_client_disconnect_all(client_registry,
                                                        disconnected_clients,
                                                        MAX_TCP_CLIENTS);
    fprintf(stderr, "[TCP Dual] Disconnected %zu clients\n", num_disconnected);

    /* Cancel all orders for disconnected clients (in both processors) */
    for (size_t i = 0; i < num_disconnected; i++) {
        processor_cancel_client_orders(&processor_ctx_0, disconnected_clients[i]);
        processor_cancel_client_orders(&processor_ctx_1, disconnected_clients[i]);
    }

    /* Let queues drain */
    fprintf(stderr, "[TCP Dual] Draining queues...\n");
    sleep(2);

    /* Join threads */
    fprintf(stderr, "[TCP Dual] Waiting for threads to finish...\n");
    pthread_join(listener_tid, NULL);
    pthread_join(processor_0_tid, NULL);
    pthread_join(processor_1_tid, NULL);
    pthread_join(router_tid, NULL);

    /* Print statistics */
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
    /* Cleanup output router (closes multicast socket if open) */
    output_router_cleanup(&router_ctx);
    
    /* Cleanup in reverse order of initialization */
    tcp_client_registry_destroy(client_registry);
    
    input_envelope_queue_destroy(input_queue_0);
    input_envelope_queue_destroy(input_queue_1);
    output_envelope_queue_destroy(output_queue_0);
    output_envelope_queue_destroy(output_queue_1);
    
    matching_engine_destroy(engine_0);
    matching_engine_destroy(engine_1);

cleanup_alloc:
    /* Free heap allocations (Power of Ten: free only at shutdown) */
    free(pools_0);
    free(pools_1);
    free(engine_0);
    free(engine_1);
    free(client_registry);
    free(input_queue_0);
    free(input_queue_1);
    free(output_queue_0);
    free(output_queue_1);

    fprintf(stderr, "\n=== TCP Dual Processor Mode Stopped ===\n");
    return result;
}

/* ============================================================================
 * TCP Mode - Single Processor Implementation
 * ============================================================================ */

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
    } else {
        fprintf(stderr, "Multicast:      Disabled\n");
    }
    fprintf(stderr, "Threads:        3 (Listener, Processor, Router)\n");
    fprintf(stderr, "========================================\n");

    int result = 0;
    
    /* ========================================================================
     * Power of Ten Rule 3 Compliant: Allocate all large structures at init
     * ======================================================================== */
    
    memory_pools_t* pools = aligned_alloc_64(sizeof(memory_pools_t));
    matching_engine_t* engine = aligned_alloc_64(sizeof(matching_engine_t));
    tcp_client_registry_t* client_registry = aligned_alloc_64(sizeof(tcp_client_registry_t));
    input_envelope_queue_t* input_queue = aligned_alloc_64(sizeof(input_envelope_queue_t));
    output_envelope_queue_t* output_queue = aligned_alloc_64(sizeof(output_envelope_queue_t));
    
    /* Rule 7: Check all allocations */
    if (!pools || !engine || !client_registry || !input_queue || !output_queue) {
        fprintf(stderr, "[TCP Single] FATAL: Failed to allocate core data structures\n");
        result = -1;
        goto cleanup_alloc;
    }
    
    fprintf(stderr, "\n✓ Core structures allocated (heap, 64-byte aligned)\n");

    /* Initialize memory pools */
    memory_pools_init(pools);
    
    fprintf(stderr, "\nMemory Pools Initialized:\n");
    fprintf(stderr, "  Order pool:      %d slots\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash table:      %d slots (open-addressing)\n", ORDER_MAP_SIZE);
    fprintf(stderr, "  Total memory:    ~%.2f MB\n",
            ((MAX_ORDERS_IN_POOL * sizeof(order_t)) +
             (ORDER_MAP_SIZE * sizeof(order_map_slot_t))) / (1024.0 * 1024.0));
    fprintf(stderr, "========================================\n\n");

    /* Initialize matching engine */
    matching_engine_init(engine, pools);

    /* Initialize client registry */
    tcp_client_registry_init(client_registry);

    /* Initialize queues */
    input_envelope_queue_init(input_queue);
    output_envelope_queue_init(output_queue);

    /* Thread contexts */
    tcp_listener_context_t listener_ctx;
    processor_t processor_ctx;
    output_router_context_t router_ctx;
    
    /* Zero-initialize contexts */
    memset(&listener_ctx, 0, sizeof(listener_ctx));
    memset(&processor_ctx, 0, sizeof(processor_ctx));
    memset(&router_ctx, 0, sizeof(router_ctx));

    /* Configure TCP listener (single mode) */
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

    /* Configure processor */
    processor_config_t processor_config = {
        .processor_id = 0,
        .tcp_mode = true,
        .spin_wait = false
    };

    if (!processor_init(&processor_ctx, &processor_config, engine,
                        input_queue, output_queue, &g_shutdown)) {
        fprintf(stderr, "[TCP Single] Failed to initialize processor\n");
        goto cleanup;
    }

    /* Configure output router (single mode) */
    output_router_config_t router_config = {
        .tcp_mode = true
    };

    if (!output_router_init(&router_ctx, &router_config, client_registry,
                            output_queue, &g_shutdown)) {
        fprintf(stderr, "[TCP Single] Failed to initialize output router\n");
        goto cleanup;
    }

    /* Enable multicast on the output router (if configured) */
    if (multicast_is_enabled(config)) {
        if (!output_router_enable_multicast(&router_ctx,
                                            config->multicast_group,
                                            config->multicast_port,
                                            1,  /* TTL = 1 for local subnet */
                                            config->binary_output)) {
            fprintf(stderr, "[TCP Single] Failed to enable multicast on router\n");
            goto cleanup;
        }
    }

    /* Create threads */
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

    fprintf(stderr, "✓ All threads started successfully\n");
    fprintf(stderr, "✓ Matching engine ready\n\n");

    if (multicast_is_enabled(config)) {
        fprintf(stderr, "💡 Multicast subscribers can connect with:\n");
        fprintf(stderr, "   ./multicast_subscriber %s %u\n\n",
                config->multicast_group, config->multicast_port);
    }

    /* Wait for shutdown signal */
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    /* Graceful shutdown */
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
    output_router_cleanup(&router_ctx);
    
    tcp_client_registry_destroy(client_registry);
    
    input_envelope_queue_destroy(input_queue);
    output_envelope_queue_destroy(output_queue);
    
    matching_engine_destroy(engine);

cleanup_alloc:
    /* Free heap allocations (Power of Ten: free only at shutdown) */
    free(pools);
    free(engine);
    free(client_registry);
    free(input_queue);
    free(output_queue);

    fprintf(stderr, "\n=== TCP Single Processor Mode Stopped ===\n");
    return result;
}

/**
 * DPDK Transport Test Program
 *
 * Tests DPDK transport layer using virtual devices (no physical NIC required).
 *
 * Build:
 *   cmake .. -DUSE_DPDK=ON -DDPDK_VDEV=null
 *   make dpdk_test
 *
 * Run:
 *   sudo ./dpdk_test
 *
 * Note: Requires sudo for DPDK EAL initialization (huge pages access)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>

#ifdef USE_DPDK

#include "network/dpdk/dpdk_init.h"
#include "network/dpdk/dpdk_config.h"
#include "network/udp_transport.h"
#include "network/multicast_transport.h"
#include "threading/queues.h"

/* Global shutdown flag */
static atomic_bool g_shutdown = false;

/* Signal handler */
static void signal_handler(int sig) {
    (void)sig;
    printf("\n[Test] Shutdown signal received\n");
    atomic_store(&g_shutdown, true);
}

/* Test input queue */
static input_envelope_queue_t g_input_queue;

/* Test output queue */
static output_envelope_queue_t g_output_queue;

/**
 * Test 1: DPDK Initialization with Virtual Device
 */
static bool test_dpdk_init(void) {
    printf("\n=== Test 1: DPDK Initialization ===\n");
    
    dpdk_config_t config;
    dpdk_config_init_vdev(&config, DPDK_VDEV_NULL);
    
    printf("[Test] Initializing DPDK with net_null virtual device...\n");
    
    if (!dpdk_init(&config)) {
        printf("[FAIL] DPDK initialization failed!\n");
        return false;
    }
    
    printf("[PASS] DPDK initialized successfully\n");
    printf("  Version: %s\n", dpdk_get_version());
    printf("  Ports available: %u\n", dpdk_get_port_count());
    printf("  Active port: %u\n", dpdk_get_active_port());
    
    /* Get mempool stats */
    uint32_t free_count, in_use;
    dpdk_get_mempool_stats(&free_count, &in_use);
    printf("  Mempool: %u free, %u in use\n", free_count, in_use);
    
    return true;
}

/**
 * Test 2: UDP Transport Creation
 */
static bool test_udp_transport(void) {
    printf("\n=== Test 2: UDP Transport ===\n");
    
    /* Initialize input queue */
    if (!input_envelope_queue_init(&g_input_queue, 1024)) {
        printf("[FAIL] Failed to create input queue\n");
        return false;
    }
    
    /* Create UDP transport config */
    udp_transport_config_t config;
    udp_transport_config_init(&config);
    config.bind_port = 12345;
    config.dual_processor = false;
    config.detect_protocol = true;
    
    printf("[Test] Creating UDP transport (port %u)...\n", config.bind_port);
    
    udp_transport_t* transport = udp_transport_create(&config,
                                                       &g_input_queue,
                                                       NULL,
                                                       &g_shutdown);
    
    if (transport == NULL) {
        printf("[FAIL] Failed to create UDP transport\n");
        return false;
    }
    
    printf("[PASS] UDP transport created\n");
    printf("  Backend: %s\n", udp_transport_get_backend());
    printf("  Port: %u\n", udp_transport_get_port(transport));
    
    /* Start transport */
    printf("[Test] Starting UDP transport...\n");
    if (!udp_transport_start(transport)) {
        printf("[FAIL] Failed to start UDP transport\n");
        udp_transport_destroy(transport);
        return false;
    }
    
    printf("[PASS] UDP transport started\n");
    
    /* Let it run briefly */
    printf("[Test] Running for 1 second...\n");
    sleep(1);
    
    /* Check stats */
    transport_stats_t stats;
    udp_transport_get_stats(transport, &stats);
    printf("  RX packets: %lu\n", stats.rx_packets);
    printf("  RX poll empty: %lu\n", stats.rx_poll_empty);
    
    /* Stop and destroy */
    printf("[Test] Stopping UDP transport...\n");
    udp_transport_stop(transport);
    udp_transport_destroy(transport);
    
    printf("[PASS] UDP transport test complete\n");
    return true;
}

/**
 * Test 3: Multicast Transport Creation
 */
static bool test_multicast_transport(void) {
    printf("\n=== Test 3: Multicast Transport ===\n");
    
    /* Initialize output queue */
    if (!output_envelope_queue_init(&g_output_queue, 1024)) {
        printf("[FAIL] Failed to create output queue\n");
        return false;
    }
    
    /* Create multicast transport config */
    multicast_transport_config_t config;
    multicast_transport_config_init(&config);
    config.group_addr = "239.255.0.1";
    config.port = 5000;
    config.use_binary = false;
    config.ttl = MULTICAST_TTL_LOCAL;
    
    printf("[Test] Creating multicast transport (%s:%u)...\n",
           config.group_addr, config.port);
    
    /* Validate multicast address */
    if (!multicast_address_is_valid(config.group_addr)) {
        printf("[FAIL] Invalid multicast address\n");
        return false;
    }
    printf("[PASS] Multicast address valid\n");
    
    multicast_transport_t* transport = multicast_transport_create(&config,
                                                                   &g_output_queue,
                                                                   NULL,
                                                                   &g_shutdown);
    
    if (transport == NULL) {
        printf("[FAIL] Failed to create multicast transport\n");
        return false;
    }
    
    printf("[PASS] Multicast transport created\n");
    printf("  Backend: %s\n", multicast_transport_get_backend());
    
    /* Start transport */
    printf("[Test] Starting multicast transport...\n");
    if (!multicast_transport_start(transport)) {
        printf("[FAIL] Failed to start multicast transport\n");
        multicast_transport_destroy(transport);
        return false;
    }
    
    printf("[PASS] Multicast transport started\n");
    
    /* Send a test message */
    printf("[Test] Sending test multicast packet...\n");
    const char* test_msg = "TEST|Hello World|12345";
    
    if (multicast_transport_send(transport, test_msg, strlen(test_msg))) {
        printf("[PASS] Test packet sent\n");
    } else {
        printf("[WARN] Test packet send failed (expected with net_null)\n");
    }
    
    /* Check stats */
    multicast_transport_stats_t stats;
    multicast_transport_get_stats(transport, &stats);
    printf("  TX packets: %lu\n", stats.tx_packets);
    printf("  TX bytes: %lu\n", stats.tx_bytes);
    printf("  Sequence: %lu\n", stats.sequence);
    
    /* Stop and destroy */
    printf("[Test] Stopping multicast transport...\n");
    multicast_transport_stop(transport);
    multicast_transport_destroy(transport);
    
    printf("[PASS] Multicast transport test complete\n");
    return true;
}

/**
 * Test 4: Statistics and Cleanup
 */
static bool test_cleanup(void) {
    printf("\n=== Test 4: Cleanup ===\n");
    
    /* Print DPDK port statistics */
    uint16_t port_id = dpdk_get_active_port();
    dpdk_print_stats(port_id);
    
    /* Cleanup DPDK */
    printf("[Test] Cleaning up DPDK...\n");
    dpdk_cleanup();
    
    if (dpdk_is_initialized()) {
        printf("[FAIL] DPDK still initialized after cleanup\n");
        return false;
    }
    
    printf("[PASS] DPDK cleanup complete\n");
    return true;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    printf("========================================\n");
    printf("DPDK Transport Test Program\n");
    printf("========================================\n");
    printf("\n");
    printf("This test uses virtual devices (net_null) to exercise\n");
    printf("the DPDK code path without requiring a physical NIC.\n");
    printf("\n");
    printf("Run with: sudo ./dpdk_test\n");
    printf("\n");
    
    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    int passed = 0;
    int failed = 0;
    
    /* Run tests */
    if (test_dpdk_init()) passed++; else failed++;
    
    if (dpdk_is_initialized()) {
        if (test_udp_transport()) passed++; else failed++;
        if (test_multicast_transport()) passed++; else failed++;
        if (test_cleanup()) passed++; else failed++;
    } else {
        printf("\n[SKIP] Skipping transport tests (DPDK init failed)\n");
        failed += 3;
    }
    
    /* Summary */
    printf("\n========================================\n");
    printf("Test Summary: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");
    
    return failed == 0 ? 0 : 1;
}

#else /* !USE_DPDK */

int main(void) {
    printf("This test requires DPDK.\n");
    printf("Build with: cmake .. -DUSE_DPDK=ON\n");
    return 1;
}

#endif /* USE_DPDK */

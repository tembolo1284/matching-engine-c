/**
 * DPDK Transport Tests
 *
 * Standalone tests (no Unity dependency) for DPDK transport.
 * Uses virtual devices (net_null) so no physical NIC is required.
 */

#ifdef USE_DPDK

#include "network/dpdk/dpdk_init.h"
#include "network/dpdk/dpdk_config.h"
#include "network/udp_transport.h"
#include "network/multicast_transport.h"
#include "network/transport_types.h"
#include "protocol/message_types.h"
#include "protocol/message_types_extended.h"
#include "threading/queues.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

/* ============================================================================
 * Simple Test Framework
 * ============================================================================ */

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 0; \
    } \
} while(0)

#define RUN_TEST(test_func) do { \
    fprintf(stderr, "Running %s... ", #test_func); \
    g_tests_run++; \
    if (test_func()) { \
        fprintf(stderr, "PASS\n"); \
        g_tests_passed++; \
    } else { \
        fprintf(stderr, "FAILED\n"); \
        g_tests_failed++; \
    } \
} while(0)

/* ============================================================================
 * Test Fixtures
 * ============================================================================ */

static atomic_bool g_shutdown = ATOMIC_VAR_INIT(false);
static input_envelope_queue_t g_input_queue;
static output_envelope_queue_t g_output_queue;

/* ============================================================================
 * DPDK Initialization Tests
 * ============================================================================ */

static int test_dpdk_init_vdev_null(void) {
    if (dpdk_is_initialized()) {
        fprintf(stderr, "(already init) ");
        return 1;  /* Pass - already initialized */
    }
    
    int ret = dpdk_init_vdev(DPDK_VDEV_NULL);
    TEST_ASSERT(ret == 0, "dpdk_init_vdev failed");
    TEST_ASSERT(dpdk_is_initialized(), "DPDK should be initialized");
    
    struct rte_mempool* mp = dpdk_get_mempool();
    TEST_ASSERT(mp != NULL, "Mempool should exist");
    
    return 1;
}

static int test_dpdk_port_status(void) {
    if (!dpdk_is_initialized()) {
        fprintf(stderr, "(skipped - not init) ");
        return 1;
    }
    
    bool link_up = false;
    uint32_t speed = 0;
    
    int ret = dpdk_port_link_status(&link_up, &speed);
    TEST_ASSERT(ret == 0, "dpdk_port_link_status failed");
    
    fprintf(stderr, "(link=%s, %uMbps) ", link_up ? "UP" : "DOWN", speed);
    return 1;
}

/* ============================================================================
 * UDP Transport Tests
 * ============================================================================ */

static int test_udp_transport_create(void) {
    if (!dpdk_is_initialized()) {
        fprintf(stderr, "(skipped) ");
        return 1;
    }
    
    atomic_store(&g_shutdown, false);
    input_envelope_queue_init(&g_input_queue);
    
    udp_transport_config_t config;
    udp_transport_config_init(&config);
    config.bind_port = 12345;
    config.dual_processor = false;
    
    udp_transport_t* transport = udp_transport_create(&config,
                                                       &g_input_queue,
                                                       NULL,
                                                       &g_shutdown);
    TEST_ASSERT(transport != NULL, "Transport creation failed");
    
    const char* backend = udp_transport_get_backend();
    TEST_ASSERT(strcmp(backend, "dpdk") == 0, "Backend should be dpdk");
    
    udp_transport_destroy(transport);
    return 1;
}

static int test_udp_transport_start_stop(void) {
    if (!dpdk_is_initialized()) {
        fprintf(stderr, "(skipped) ");
        return 1;
    }
    
    atomic_store(&g_shutdown, false);
    input_envelope_queue_init(&g_input_queue);
    
    udp_transport_config_t config;
    udp_transport_config_init(&config);
    config.bind_port = 12346;
    
    udp_transport_t* transport = udp_transport_create(&config,
                                                       &g_input_queue,
                                                       NULL,
                                                       &g_shutdown);
    TEST_ASSERT(transport != NULL, "Transport creation failed");
    
    bool started = udp_transport_start(transport);
    TEST_ASSERT(started, "Transport failed to start");
    TEST_ASSERT(udp_transport_is_running(transport), "Transport should be running");
    
    usleep(10000);  /* 10ms */
    
    atomic_store(&g_shutdown, true);
    udp_transport_stop(transport);
    TEST_ASSERT(!udp_transport_is_running(transport), "Transport should be stopped");
    
    udp_transport_destroy(transport);
    return 1;
}

/* ============================================================================
 * Multicast Transport Tests
 * ============================================================================ */

static int test_multicast_transport_create(void) {
    if (!dpdk_is_initialized()) {
        fprintf(stderr, "(skipped) ");
        return 1;
    }
    
    atomic_store(&g_shutdown, false);
    output_envelope_queue_init(&g_output_queue);
    
    multicast_transport_config_t config;
    multicast_transport_config_init(&config);
    config.group_addr = "239.255.0.1";
    config.port = 5000;
    
    multicast_transport_t* transport = multicast_transport_create(&config,
                                                                   &g_output_queue,
                                                                   NULL,
                                                                   &g_shutdown);
    TEST_ASSERT(transport != NULL, "Transport creation failed");
    
    const char* backend = multicast_transport_get_backend();
    TEST_ASSERT(strcmp(backend, "dpdk") == 0, "Backend should be dpdk");
    
    multicast_transport_destroy(transport);
    return 1;
}

static int test_multicast_transport_start_stop(void) {
    if (!dpdk_is_initialized()) {
        fprintf(stderr, "(skipped) ");
        return 1;
    }
    
    atomic_store(&g_shutdown, false);
    output_envelope_queue_init(&g_output_queue);
    
    multicast_transport_config_t config;
    multicast_transport_config_init(&config);
    config.group_addr = "239.255.0.1";
    config.port = 5001;
    
    multicast_transport_t* transport = multicast_transport_create(&config,
                                                                   &g_output_queue,
                                                                   NULL,
                                                                   &g_shutdown);
    TEST_ASSERT(transport != NULL, "Transport creation failed");
    
    bool started = multicast_transport_start(transport);
    TEST_ASSERT(started, "Transport failed to start");
    TEST_ASSERT(multicast_transport_is_running(transport), "Transport should be running");
    
    usleep(10000);
    
    atomic_store(&g_shutdown, true);
    multicast_transport_stop(transport);
    TEST_ASSERT(!multicast_transport_is_running(transport), "Transport should be stopped");
    
    multicast_transport_destroy(transport);
    return 1;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

static int test_dpdk_cleanup(void) {
    if (!dpdk_is_initialized()) {
        return 1;
    }
    
    dpdk_cleanup();
    TEST_ASSERT(!dpdk_is_initialized(), "DPDK should be cleaned up");
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    fprintf(stderr, "\n=== DPDK Transport Tests ===\n\n");
    
    RUN_TEST(test_dpdk_init_vdev_null);
    RUN_TEST(test_dpdk_port_status);
    RUN_TEST(test_udp_transport_create);
    RUN_TEST(test_udp_transport_start_stop);
    RUN_TEST(test_multicast_transport_create);
    RUN_TEST(test_multicast_transport_start_stop);
    RUN_TEST(test_dpdk_cleanup);
    
    fprintf(stderr, "\n=== Results ===\n");
    fprintf(stderr, "Tests run:    %d\n", g_tests_run);
    fprintf(stderr, "Tests passed: %d\n", g_tests_passed);
    fprintf(stderr, "Tests failed: %d\n", g_tests_failed);
    
    return g_tests_failed > 0 ? 1 : 0;
}

#else /* !USE_DPDK */

#include <stdio.h>

int main(void) {
    fprintf(stderr, "DPDK tests disabled (USE_DPDK=0)\n");
    fprintf(stderr, "Build with: cmake .. -DUSE_DPDK=ON\n");
    return 0;
}

#endif /* USE_DPDK */

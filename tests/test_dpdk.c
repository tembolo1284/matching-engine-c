/**
 * DPDK Transport Tests
 *
 * Tests for DPDK UDP and multicast transport implementations.
 * Uses virtual devices (net_null) so no physical NIC is required.
 */

#ifdef USE_DPDK

#include "unity.h"
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

/* ============================================================================
 * Test Fixtures
 * ============================================================================ */

static atomic_bool g_shutdown = ATOMIC_VAR_INIT(false);
static input_envelope_queue_t g_input_queue;
static output_envelope_queue_t g_output_queue;

void setUp(void) {
    atomic_store(&g_shutdown, false);
}

void tearDown(void) {
    atomic_store(&g_shutdown, true);
}

/* ============================================================================
 * DPDK Initialization Tests
 * ============================================================================ */

void test_dpdk_init_vdev_null(void) {
    /* Skip if DPDK already initialized */
    if (dpdk_is_initialized()) {
        TEST_PASS();
        return;
    }
    
    int ret = dpdk_init_vdev(DPDK_VDEV_NULL);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_TRUE(dpdk_is_initialized());
    
    /* Check we have a mempool */
    struct rte_mempool* mp = dpdk_get_mempool();
    TEST_ASSERT_NOT_NULL(mp);
    
    /* Print stats */
    dpdk_print_stats();
}

void test_dpdk_port_status(void) {
    if (!dpdk_is_initialized()) {
        TEST_IGNORE();
        return;
    }
    
    bool link_up = false;
    uint32_t speed = 0;
    
    int ret = dpdk_port_link_status(&link_up, &speed);
    TEST_ASSERT_EQUAL_INT(0, ret);
    
    /* net_null typically shows link up at 10G */
    fprintf(stderr, "Link: %s, Speed: %u Mbps\n",
            link_up ? "UP" : "DOWN", speed);
}

/* ============================================================================
 * UDP Transport Tests
 * ============================================================================ */

void test_udp_transport_create(void) {
    if (!dpdk_is_initialized()) {
        TEST_IGNORE();
        return;
    }
    
    /* Initialize queue */
    input_envelope_queue_init(&g_input_queue);
    
    udp_transport_config_t config;
    udp_transport_config_init(&config);
    config.bind_port = 12345;
    config.dual_processor = false;
    
    udp_transport_t* transport = udp_transport_create(&config,
                                                       &g_input_queue,
                                                       NULL,
                                                       &g_shutdown);
    TEST_ASSERT_NOT_NULL(transport);
    
    /* Check backend */
    const char* backend = udp_transport_get_backend();
    TEST_ASSERT_EQUAL_STRING("dpdk", backend);
    
    /* Cleanup */
    udp_transport_destroy(transport);
}

void test_udp_transport_start_stop(void) {
    if (!dpdk_is_initialized()) {
        TEST_IGNORE();
        return;
    }
    
    input_envelope_queue_init(&g_input_queue);
    
    udp_transport_config_t config;
    udp_transport_config_init(&config);
    config.bind_port = 12346;
    
    udp_transport_t* transport = udp_transport_create(&config,
                                                       &g_input_queue,
                                                       NULL,
                                                       &g_shutdown);
    TEST_ASSERT_NOT_NULL(transport);
    
    /* Start */
    bool started = udp_transport_start(transport);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(udp_transport_is_running(transport));
    
    /* Let it run briefly */
    usleep(10000);  /* 10ms */
    
    /* Stop */
    atomic_store(&g_shutdown, true);
    udp_transport_stop(transport);
    TEST_ASSERT_FALSE(udp_transport_is_running(transport));
    
    /* Cleanup */
    udp_transport_destroy(transport);
}

/* ============================================================================
 * Multicast Transport Tests
 * ============================================================================ */

void test_multicast_transport_create(void) {
    if (!dpdk_is_initialized()) {
        TEST_IGNORE();
        return;
    }
    
    output_envelope_queue_init(&g_output_queue);
    
    multicast_transport_config_t config;
    multicast_transport_config_init(&config);
    config.group_addr = "239.255.0.1";
    config.port = 5000;
    
    multicast_transport_t* transport = multicast_transport_create(&config,
                                                                   &g_output_queue,
                                                                   NULL,
                                                                   &g_shutdown);
    TEST_ASSERT_NOT_NULL(transport);
    
    /* Check backend */
    const char* backend = multicast_transport_get_backend();
    TEST_ASSERT_EQUAL_STRING("dpdk", backend);
    
    /* Cleanup */
    multicast_transport_destroy(transport);
}

void test_multicast_transport_start_stop(void) {
    if (!dpdk_is_initialized()) {
        TEST_IGNORE();
        return;
    }
    
    output_envelope_queue_init(&g_output_queue);
    
    multicast_transport_config_t config;
    multicast_transport_config_init(&config);
    config.group_addr = "239.255.0.1";
    config.port = 5001;
    
    multicast_transport_t* transport = multicast_transport_create(&config,
                                                                   &g_output_queue,
                                                                   NULL,
                                                                   &g_shutdown);
    TEST_ASSERT_NOT_NULL(transport);
    
    /* Start */
    bool started = multicast_transport_start(transport);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(multicast_transport_is_running(transport));
    
    /* Let it run briefly */
    usleep(10000);
    
    /* Stop */
    atomic_store(&g_shutdown, true);
    multicast_transport_stop(transport);
    TEST_ASSERT_FALSE(multicast_transport_is_running(transport));
    
    /* Cleanup */
    multicast_transport_destroy(transport);
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

void test_dpdk_cleanup(void) {
    if (!dpdk_is_initialized()) {
        TEST_PASS();
        return;
    }
    
    dpdk_cleanup();
    TEST_ASSERT_FALSE(dpdk_is_initialized());
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    UNITY_BEGIN();
    
    /* Run tests in order */
    RUN_TEST(test_dpdk_init_vdev_null);
    RUN_TEST(test_dpdk_port_status);
    RUN_TEST(test_udp_transport_create);
    RUN_TEST(test_udp_transport_start_stop);
    RUN_TEST(test_multicast_transport_create);
    RUN_TEST(test_multicast_transport_start_stop);
    RUN_TEST(test_dpdk_cleanup);
    
    return UNITY_END();
}

#else /* !USE_DPDK */

#include <stdio.h>

int main(void) {
    fprintf(stderr, "DPDK tests disabled (USE_DPDK=0)\n");
    fprintf(stderr, "Build with: cmake .. -DUSE_DPDK=ON\n");
    return 0;
}

#endif /* USE_DPDK */

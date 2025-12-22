#ifndef DPDK_INIT_H
#define DPDK_INIT_H

#include "network/dpdk/dpdk_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DPDK Initialization Module
 *
 * Handles DPDK EAL initialization, port configuration, and mempool setup.
 * This module provides a simplified interface for setting up DPDK for
 * UDP/multicast packet I/O.
 */

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

struct rte_mempool;

/* ============================================================================
 * Global State (set during init, read-only after)
 * ============================================================================ */

/**
 * Get the global mempool for packet buffers
 *
 * @return Pointer to mempool, or NULL if not initialized
 */
struct rte_mempool* dpdk_get_mempool(void);

/**
 * Get the configured port ID
 *
 * @return Port ID (0 if using virtual device)
 */
uint16_t dpdk_get_port_id(void);

/**
 * Check if DPDK has been initialized
 *
 * @return true if dpdk_init() has been called successfully
 */
bool dpdk_is_initialized(void);

/* ============================================================================
 * Initialization API
 * ============================================================================ */

/**
 * Initialize DPDK environment
 *
 * This function:
 * 1. Initializes EAL with provided arguments
 * 2. Creates packet mempool
 * 3. Configures the specified port (or virtual device)
 * 4. Starts the port
 *
 * @param config Configuration structure
 * @return 0 on success, negative error code on failure
 *
 * Error codes:
 *   -1: EAL init failed
 *   -2: No ports available
 *   -3: Mempool creation failed
 *   -4: Port configuration failed
 *   -5: RX queue setup failed
 *   -6: TX queue setup failed
 *   -7: Port start failed
 */
int dpdk_init(const dpdk_config_t* config);

/**
 * Initialize DPDK with default configuration
 *
 * Convenience function that uses default settings.
 *
 * @return 0 on success, negative error code on failure
 */
int dpdk_init_default(void);

/**
 * Initialize DPDK with virtual device (for testing)
 *
 * Uses net_null or net_ring virtual device for testing
 * without a physical NIC.
 *
 * @param vdev_type Type of virtual device
 * @return 0 on success, negative error code on failure
 */
int dpdk_init_vdev(dpdk_vdev_type_t vdev_type);

/**
 * Cleanup DPDK resources
 *
 * Stops port, frees mempool, and cleans up EAL.
 * Should be called before program exit.
 */
void dpdk_cleanup(void);

/* ============================================================================
 * Port Control
 * ============================================================================ */

/**
 * Stop the DPDK port
 *
 * Stops packet reception/transmission on the port.
 */
void dpdk_port_stop(void);

/**
 * Start the DPDK port
 *
 * Resumes packet reception/transmission.
 *
 * @return 0 on success, negative on failure
 */
int dpdk_port_start(void);

/**
 * Get port link status
 *
 * @param link_up Output: true if link is up
 * @param speed_mbps Output: link speed in Mbps
 * @return 0 on success, negative on failure
 */
int dpdk_port_link_status(bool* link_up, uint32_t* speed_mbps);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Get port statistics
 *
 * @param stats Output structure for statistics
 * @return 0 on success, negative on failure
 */
int dpdk_get_stats(dpdk_stats_t* stats);

/**
 * Reset port statistics
 *
 * @return 0 on success, negative on failure
 */
int dpdk_reset_stats(void);

/**
 * Print port statistics to stderr
 */
void dpdk_print_stats(void);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get DPDK version string
 *
 * @return Version string (e.g., "DPDK 21.11.0")
 */
const char* dpdk_version(void);

/**
 * Get error string for error code
 *
 * @param errnum Error code from dpdk_init()
 * @return Human-readable error string
 */
const char* dpdk_strerror(int errnum);

#ifdef __cplusplus
}
#endif

#endif /* DPDK_INIT_H */#ifndef DPDK_INIT_H
#define DPDK_INIT_H

#include "network/dpdk/dpdk_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DPDK Initialization Module
 *
 * Handles DPDK EAL initialization, port configuration, and mempool setup.
 * This module provides a simplified interface for setting up DPDK for
 * UDP/multicast packet I/O.
 */

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

struct rte_mempool;

/* ============================================================================
 * Global State (set during init, read-only after)
 * ============================================================================ */

/**
 * Get the global mempool for packet buffers
 *
 * @return Pointer to mempool, or NULL if not initialized
 */
struct rte_mempool* dpdk_get_mempool(void);

/**
 * Get the configured port ID
 *
 * @return Port ID (0 if using virtual device)
 */
uint16_t dpdk_get_port_id(void);

/**
 * Check if DPDK has been initialized
 *
 * @return true if dpdk_init() has been called successfully
 */
bool dpdk_is_initialized(void);

/* ============================================================================
 * Initialization API
 * ============================================================================ */

/**
 * Initialize DPDK environment
 *
 * This function:
 * 1. Initializes EAL with provided arguments
 * 2. Creates packet mempool
 * 3. Configures the specified port (or virtual device)
 * 4. Starts the port
 *
 * @param config Configuration structure
 * @return 0 on success, negative error code on failure
 *
 * Error codes:
 *   -1: EAL init failed
 *   -2: No ports available
 *   -3: Mempool creation failed
 *   -4: Port configuration failed
 *   -5: RX queue setup failed
 *   -6: TX queue setup failed
 *   -7: Port start failed
 */
int dpdk_init(const dpdk_config_t* config);

/**
 * Initialize DPDK with default configuration
 *
 * Convenience function that uses default settings.
 *
 * @return 0 on success, negative error code on failure
 */
int dpdk_init_default(void);

/**
 * Initialize DPDK with virtual device (for testing)
 *
 * Uses net_null or net_ring virtual device for testing
 * without a physical NIC.
 *
 * @param vdev_type Type of virtual device
 * @return 0 on success, negative error code on failure
 */
int dpdk_init_vdev(dpdk_vdev_type_t vdev_type);

/**
 * Cleanup DPDK resources
 *
 * Stops port, frees mempool, and cleans up EAL.
 * Should be called before program exit.
 */
void dpdk_cleanup(void);

/* ============================================================================
 * Port Control
 * ============================================================================ */

/**
 * Stop the DPDK port
 *
 * Stops packet reception/transmission on the port.
 */
void dpdk_port_stop(void);

/**
 * Start the DPDK port
 *
 * Resumes packet reception/transmission.
 *
 * @return 0 on success, negative on failure
 */
int dpdk_port_start(void);

/**
 * Get port link status
 *
 * @param link_up Output: true if link is up
 * @param speed_mbps Output: link speed in Mbps
 * @return 0 on success, negative on failure
 */
int dpdk_port_link_status(bool* link_up, uint32_t* speed_mbps);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Get port statistics
 *
 * @param stats Output structure for statistics
 * @return 0 on success, negative on failure
 */
int dpdk_get_stats(dpdk_stats_t* stats);

/**
 * Reset port statistics
 *
 * @return 0 on success, negative on failure
 */
int dpdk_reset_stats(void);

/**
 * Print port statistics to stderr
 */
void dpdk_print_stats(void);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get DPDK version string
 *
 * @return Version string (e.g., "DPDK 21.11.0")
 */
const char* dpdk_version(void);

/**
 * Get error string for error code
 *
 * @param errnum Error code from dpdk_init()
 * @return Human-readable error string
 */
const char* dpdk_strerror(int errnum);

#ifdef __cplusplus
}
#endif

#endif /* DPDK_INIT_H */

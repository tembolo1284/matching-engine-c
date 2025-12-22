#ifndef DPDK_INIT_H
#define DPDK_INIT_H

#include "network/dpdk/dpdk_config.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DPDK Initialization API
 *
 * This module handles:
 *   - EAL (Environment Abstraction Layer) initialization
 *   - Memory pool creation for packet buffers
 *   - Port configuration and startup
 *   - Virtual device setup for testing
 *
 * Usage:
 *   dpdk_config_t config;
 *   dpdk_config_init(&config);
 *   config.vdev_type = DPDK_VDEV_NULL;  // For testing without NIC
 *
 *   if (!dpdk_init(&config)) {
 *       // Handle error
 *   }
 *
 *   // ... use DPDK ...
 *
 *   dpdk_cleanup();
 */

/* ============================================================================
 * Initialization / Cleanup
 * ============================================================================ */

/**
 * Initialize DPDK environment
 *
 * Must be called before any other DPDK functions.
 * Initializes EAL, creates mempool, configures ports.
 *
 * @param config Configuration (NULL = use defaults)
 * @return true on success, false on error
 *
 * Postconditions on success:
 * - EAL initialized
 * - Mempool created
 * - Port(s) configured and started
 */
bool dpdk_init(const dpdk_config_t* config);

/**
 * Cleanup DPDK environment
 *
 * Stops ports, frees mempool, cleans up EAL.
 * Safe to call multiple times.
 */
void dpdk_cleanup(void);

/**
 * Check if DPDK is initialized
 */
bool dpdk_is_initialized(void);

/* ============================================================================
 * Port Information
 * ============================================================================ */

/**
 * Get number of available ports
 */
uint16_t dpdk_get_port_count(void);

/**
 * Get the active port ID
 *
 * @return Port ID, or UINT16_MAX if no port configured
 */
uint16_t dpdk_get_active_port(void);

/**
 * Check if port is up and running
 */
bool dpdk_port_is_up(uint16_t port_id);

/**
 * Get port MAC address
 *
 * @param port_id Port ID
 * @param mac_out Output: 6-byte MAC address
 * @return true on success
 */
bool dpdk_get_port_mac(uint16_t port_id, uint8_t mac_out[6]);

/* ============================================================================
 * Mempool Access
 * ============================================================================ */

/**
 * Opaque mempool handle
 *
 * In real DPDK code, this would be struct rte_mempool*.
 * We use void* for the header to avoid requiring DPDK headers.
 */
typedef void* dpdk_mempool_t;

/**
 * Get the packet mempool
 *
 * Used for allocating packet buffers (mbufs).
 *
 * @return Mempool handle, or NULL if not initialized
 */
dpdk_mempool_t dpdk_get_mempool(void);

/**
 * Get mempool statistics
 *
 * @param free_count Output: number of free mbufs
 * @param in_use_count Output: number of mbufs in use
 */
void dpdk_get_mempool_stats(uint32_t* free_count, uint32_t* in_use_count);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Get port statistics
 *
 * @param port_id Port ID
 * @param stats Output: statistics structure
 * @return true on success
 */
bool dpdk_get_port_stats(uint16_t port_id, dpdk_stats_t* stats);

/**
 * Reset port statistics
 */
void dpdk_reset_port_stats(uint16_t port_id);

/**
 * Print port statistics to stderr
 */
void dpdk_print_stats(uint16_t port_id);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get DPDK version string
 */
const char* dpdk_get_version(void);

/**
 * Convert MAC address to string
 *
 * @param mac 6-byte MAC address
 * @param buf Output buffer (at least 18 bytes)
 * @return buf
 */
char* dpdk_mac_to_str(const uint8_t mac[6], char* buf);

/**
 * Get lcore ID for current thread
 *
 * @return lcore ID, or UINT32_MAX if not an EAL thread
 */
uint32_t dpdk_get_lcore_id(void);

#ifdef __cplusplus
}
#endif

#endif /* DPDK_INIT_H */#ifndef DPDK_INIT_H
#define DPDK_INIT_H

#include "network/dpdk/dpdk_config.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DPDK Initialization API
 *
 * This module handles:
 *   - EAL (Environment Abstraction Layer) initialization
 *   - Memory pool creation for packet buffers
 *   - Port configuration and startup
 *   - Virtual device setup for testing
 *
 * Usage:
 *   dpdk_config_t config;
 *   dpdk_config_init(&config);
 *   config.vdev_type = DPDK_VDEV_NULL;  // For testing without NIC
 *
 *   if (!dpdk_init(&config)) {
 *       // Handle error
 *   }
 *
 *   // ... use DPDK ...
 *
 *   dpdk_cleanup();
 */

/* ============================================================================
 * Initialization / Cleanup
 * ============================================================================ */

/**
 * Initialize DPDK environment
 *
 * Must be called before any other DPDK functions.
 * Initializes EAL, creates mempool, configures ports.
 *
 * @param config Configuration (NULL = use defaults)
 * @return true on success, false on error
 *
 * Postconditions on success:
 * - EAL initialized
 * - Mempool created
 * - Port(s) configured and started
 */
bool dpdk_init(const dpdk_config_t* config);

/**
 * Cleanup DPDK environment
 *
 * Stops ports, frees mempool, cleans up EAL.
 * Safe to call multiple times.
 */
void dpdk_cleanup(void);

/**
 * Check if DPDK is initialized
 */
bool dpdk_is_initialized(void);

/* ============================================================================
 * Port Information
 * ============================================================================ */

/**
 * Get number of available ports
 */
uint16_t dpdk_get_port_count(void);

/**
 * Get the active port ID
 *
 * @return Port ID, or UINT16_MAX if no port configured
 */
uint16_t dpdk_get_active_port(void);

/**
 * Check if port is up and running
 */
bool dpdk_port_is_up(uint16_t port_id);

/**
 * Get port MAC address
 *
 * @param port_id Port ID
 * @param mac_out Output: 6-byte MAC address
 * @return true on success
 */
bool dpdk_get_port_mac(uint16_t port_id, uint8_t mac_out[6]);

/* ============================================================================
 * Mempool Access
 * ============================================================================ */

/**
 * Opaque mempool handle
 *
 * In real DPDK code, this would be struct rte_mempool*.
 * We use void* for the header to avoid requiring DPDK headers.
 */
typedef void* dpdk_mempool_t;

/**
 * Get the packet mempool
 *
 * Used for allocating packet buffers (mbufs).
 *
 * @return Mempool handle, or NULL if not initialized
 */
dpdk_mempool_t dpdk_get_mempool(void);

/**
 * Get mempool statistics
 *
 * @param free_count Output: number of free mbufs
 * @param in_use_count Output: number of mbufs in use
 */
void dpdk_get_mempool_stats(uint32_t* free_count, uint32_t* in_use_count);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * Get port statistics
 *
 * @param port_id Port ID
 * @param stats Output: statistics structure
 * @return true on success
 */
bool dpdk_get_port_stats(uint16_t port_id, dpdk_stats_t* stats);

/**
 * Reset port statistics
 */
void dpdk_reset_port_stats(uint16_t port_id);

/**
 * Print port statistics to stderr
 */
void dpdk_print_stats(uint16_t port_id);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get DPDK version string
 */
const char* dpdk_get_version(void);

/**
 * Convert MAC address to string
 *
 * @param mac 6-byte MAC address
 * @param buf Output buffer (at least 18 bytes)
 * @return buf
 */
char* dpdk_mac_to_str(const uint8_t mac[6], char* buf);

/**
 * Get lcore ID for current thread
 *
 * @return lcore ID, or UINT32_MAX if not an EAL thread
 */
uint32_t dpdk_get_lcore_id(void);

#ifdef __cplusplus
}
#endif

#endif /* DPDK_INIT_H */

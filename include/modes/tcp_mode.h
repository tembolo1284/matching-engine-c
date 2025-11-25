#ifndef TCP_MODE_H
#define TCP_MODE_H

#include "modes/run_modes.h"

/**
 * Run TCP mode with dual processors (A-M / N-Z symbol partitioning)
 * 
 * Threads:
 *   - TCP Listener (routes by symbol)
 *   - Processor 0 (symbols A-M)
 *   - Processor 1 (symbols N-Z)
 *   - Output Router (round-robin to TCP clients)
 *   - Optional: Multicast Publisher (if enabled)
 * 
 * @param config Application configuration
 * @return 0 on success, non-zero on error
 */
int run_tcp_dual_processor(const app_config_t* config);

/**
 * Run TCP mode with single processor (all symbols)
 * 
 * Threads:
 *   - TCP Listener
 *   - Processor (all symbols)
 *   - Output Router
 *   - Optional: Multicast Publisher (if enabled)
 * 
 * @param config Application configuration
 * @return 0 on success, non-zero on error
 */
int run_tcp_single_processor(const app_config_t* config);

#endif // TCP_MODE_H

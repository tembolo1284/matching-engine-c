#ifndef UDP_MODE_H
#define UDP_MODE_H

#include "modes/run_modes.h"

/**
 * Run UDP mode with dual processors (A-M / N-Z symbol partitioning)
 * 
 * Threads:
 *   - UDP Receiver (routes by symbol)
 *   - Processor 0 (symbols A-M)
 *   - Processor 1 (symbols N-Z)
 *   - Output Publisher (stdout)
 * 
 * Note: Currently outputs only processor 0 results
 *       Use TCP mode for full dual-processor output support
 * 
 * @param config Application configuration
 * @return 0 on success, non-zero on error
 */
int run_udp_dual_processor(const app_config_t* config);

/**
 * Run UDP mode with single processor (all symbols)
 * 
 * Threads:
 *   - UDP Receiver
 *   - Processor (all symbols)
 *   - Output Publisher (stdout)
 * 
 * @param config Application configuration
 * @return 0 on success, non-zero on error
 */
int run_udp_single_processor(const app_config_t* config);

#endif // UDP_MODE_H

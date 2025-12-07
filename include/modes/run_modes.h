#ifndef RUN_MODES_H
#define RUN_MODES_H

#include <stdint.h>
#include <stdbool.h>
#include "core/order_book.h"

/**
 * Application configuration
 */
typedef struct {
    bool tcp_mode;              // true = TCP, false = UDP
    uint16_t port;              // Network port
    bool binary_output;         // true = binary, false = CSV
    bool dual_processor;        // true = dual-processor, false = single
    bool quiet_mode;            // true = suppress per-message output (benchmark mode)
    bool enable_multicast;      // true = broadcast to multicast group
    char multicast_group[64];   // Multicast group address (e.g., "239.255.0.1")
    uint16_t multicast_port;    // Multicast port (e.g., 5000)
} app_config_t;

/**
 * Helper: Print memory pool statistics
 */
void print_memory_stats(const char* label, const memory_pools_t* pools);

#endif // RUN_MODES_H

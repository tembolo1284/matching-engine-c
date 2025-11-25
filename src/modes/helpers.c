#include "modes/run_modes.h"
#include "core/order_book.h"

#include <stdio.h>

/**
 * Print memory pool statistics
 */
void print_memory_stats(const char* label, const memory_pools_t* pools) {
    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, &stats);
    
    fprintf(stderr, "\n--- %s Memory Pool Statistics ---\n", label);
    fprintf(stderr, "Order Pool:\n");
    fprintf(stderr, "  Total allocations: %u\n", stats.order_allocations);
    fprintf(stderr, "  Peak usage:        %u / %u (%.1f%%)\n", 
            stats.order_peak_usage, MAX_ORDERS_IN_POOL,
            (stats.order_peak_usage * 100.0) / MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Failures:          %u\n", stats.order_failures);
    
    fprintf(stderr, "Hash Entry Pool:\n");
    fprintf(stderr, "  Total allocations: %u\n", stats.hash_allocations);
    fprintf(stderr, "  Peak usage:        %u / %u (%.1f%%)\n",
            stats.hash_peak_usage, MAX_HASH_ENTRIES_IN_POOL,
            (stats.hash_peak_usage * 100.0) / MAX_HASH_ENTRIES_IN_POOL);
    fprintf(stderr, "  Failures:          %u\n", stats.hash_failures);
}

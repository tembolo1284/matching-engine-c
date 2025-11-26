#include "modes/run_modes.h"
#include "core/order_book.h"

#include <stdio.h>

/**
 * Print memory pool statistics
 */
void print_memory_stats(const char* label, const memory_pools_t* pools) {
    memory_pool_stats_t stats;
    
    // Note: With open-addressing hash tables, we only track order pool stats
    // The hash table is now inline and doesn't use a separate pool
    
    // Get stats without order_book (pass NULL - stats will only have order pool info)
    memory_pools_get_stats(pools, NULL, &stats);
    
    fprintf(stderr, "\n=== Memory Pool Statistics ===\n");
    
    fprintf(stderr, "Order Pool:\n");
    fprintf(stderr, "  Total allocations: %u\n", stats.order_allocations);
    fprintf(stderr, "  Peak usage:        %u / %d (%.1f%%)\n",
            stats.order_peak_usage, MAX_ORDERS_IN_POOL,
            (stats.order_peak_usage * 100.0) / MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Failures:          %u\n", stats.order_failures);
    
    // Hash entry pool was removed - now using open-addressing inline hash table
    fprintf(stderr, "Hash Table: Using open-addressing (no separate pool)\n");
}

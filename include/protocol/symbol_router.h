#ifndef SYMBOL_ROUTER_H
#define SYMBOL_ROUTER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Symbol Router - Routes messages to processors based on symbol
 * 
 * Partitioning scheme:
 *   - Symbols starting with A-M → Processor 0
 *   - Symbols starting with N-Z → Processor 1
 *   - Empty/invalid symbols → Processor 0 (default)
 * 
 * This provides roughly balanced load for typical symbol distributions.
 */

#define NUM_PROCESSORS 2
#define PROCESSOR_ID_A_TO_M 0
#define PROCESSOR_ID_N_TO_Z 1

/**
 * Get processor ID for a given symbol
 * 
 * @param symbol The trading symbol (e.g., "AAPL", "NVDA")
 * @return 0 for A-M symbols, 1 for N-Z symbols
 */
static inline int get_processor_id_for_symbol(const char* symbol) {
    if (symbol == NULL || symbol[0] == '\0') {
        return PROCESSOR_ID_A_TO_M;  // Default to processor 0
    }
    
    char first = symbol[0];
    
    // Normalize to uppercase
    if (first >= 'a' && first <= 'z') {
        first = first - 'a' + 'A';
    }
    
    // A-M (65-77) → Processor 0
    // N-Z (78-90) → Processor 1
    // Non-alphabetic → Processor 0 (default)
    if (first >= 'A' && first <= 'M') {
        return PROCESSOR_ID_A_TO_M;
    } else if (first >= 'N' && first <= 'Z') {
        return PROCESSOR_ID_N_TO_Z;
    } else {
        return PROCESSOR_ID_A_TO_M;  // Default for numeric/special symbols
    }
}

/**
 * Check if a symbol string is valid (non-empty)
 */
static inline bool symbol_is_valid(const char* symbol) {
    return symbol != NULL && symbol[0] != '\0';
}

/**
 * Get human-readable name for processor
 */
static inline const char* get_processor_name(int processor_id) {
    switch (processor_id) {
        case PROCESSOR_ID_A_TO_M: return "A-M";
        case PROCESSOR_ID_N_TO_Z: return "N-Z";
        default: return "Unknown";
    }
}

#ifdef __cplusplus
}
#endif

#endif // SYMBOL_ROUTER_H

#ifndef SYMBOL_ROUTER_H
#define SYMBOL_ROUTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
 * The routing is deterministic - same symbol always goes to same processor.
 */

#define NUM_PROCESSORS 2
#define PROCESSOR_ID_A_TO_M 0
#define PROCESSOR_ID_N_TO_Z 1

/**
 * Get processor ID for a given symbol
 * 
 * Uses a branchless implementation for better pipeline behavior.
 * 
 * @param symbol The trading symbol (e.g., "AAPL", "NVDA")
 * @return 0 for A-M symbols, 1 for N-Z symbols
 */
static inline int get_processor_id_for_symbol(const char* symbol) {
    /* Handle NULL/empty - default to processor 0 */
    if (symbol == NULL || symbol[0] == '\0') {
        return PROCESSOR_ID_A_TO_M;
    }
    
    /* Get first character and normalize to uppercase */
    unsigned char first = (unsigned char)symbol[0];
    
    /* Convert lowercase to uppercase (branchless) */
    /* If 'a' <= first <= 'z', subtract 32 to get uppercase */
    unsigned char is_lower = (first >= 'a') & (first <= 'z');
    first -= is_lower * ('a' - 'A');
    
    /* Check if in A-M range (65-77) or N-Z range (78-90) */
    /* A-M: first >= 'A' && first <= 'M' → processor 0 */
    /* N-Z: first >= 'N' && first <= 'Z' → processor 1 */
    /* Other: processor 0 (default) */
    
    unsigned char is_n_to_z = (first >= 'N') & (first <= 'Z');
    
    return is_n_to_z ? PROCESSOR_ID_N_TO_Z : PROCESSOR_ID_A_TO_M;
}

/**
 * Check if a symbol string is valid (non-empty, starts with letter)
 */
static inline bool symbol_is_valid(const char* symbol) {
    if (symbol == NULL || symbol[0] == '\0') {
        return false;
    }
    
    unsigned char first = (unsigned char)symbol[0];
    
    /* Check if alphabetic (A-Z or a-z) */
    return ((first >= 'A') & (first <= 'Z')) | 
           ((first >= 'a') & (first <= 'z'));
}

/**
 * Get human-readable name for processor
 */
static inline const char* get_processor_name(int processor_id) {
    static const char* names[] = { "A-M", "N-Z", "Unknown" };
    
    if (processor_id >= 0 && processor_id < NUM_PROCESSORS) {
        return names[processor_id];
    }
    return names[2];
}

/**
 * Get processor ID using only the first character (for binary protocol)
 * This avoids needing the full symbol string.
 */
static inline int get_processor_id_for_char(char first) {
    /* Handle empty */
    if (first == '\0') {
        return PROCESSOR_ID_A_TO_M;
    }
    
    unsigned char c = (unsigned char)first;
    
    /* Normalize to uppercase (branchless) */
    unsigned char is_lower = (c >= 'a') & (c <= 'z');
    c -= is_lower * ('a' - 'A');
    
    /* N-Z check */
    unsigned char is_n_to_z = (c >= 'N') & (c <= 'Z');
    
    return is_n_to_z ? PROCESSOR_ID_N_TO_Z : PROCESSOR_ID_A_TO_M;
}

#ifdef __cplusplus
}
#endif

#endif /* SYMBOL_ROUTER_H */

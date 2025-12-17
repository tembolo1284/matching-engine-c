#ifndef SYMBOL_ROUTER_H
#define SYMBOL_ROUTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

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
 *
 * Implementation uses branchless arithmetic for consistent pipeline behavior.
 */

/* ============================================================================
 * Constants
 * ============================================================================ */

#define NUM_PROCESSORS 2
#define PROCESSOR_ID_A_TO_M 0
#define PROCESSOR_ID_N_TO_Z 1

/* Compile-time verification */
_Static_assert(PROCESSOR_ID_A_TO_M == 0, "A-M processor must be 0 for branchless math");
_Static_assert(PROCESSOR_ID_N_TO_Z == 1, "N-Z processor must be 1 for branchless math");

/* ============================================================================
 * Core Routing Functions
 * ============================================================================ */

/**
 * Get processor ID for a given symbol (branchless)
 *
 * Uses branchless arithmetic for consistent pipeline behavior:
 * - No branch misprediction penalty
 * - Constant-time execution regardless of input
 *
 * @param symbol The trading symbol (e.g., "AAPL", "NVDA")
 * @return 0 for A-M symbols (or invalid), 1 for N-Z symbols
 *
 * Preconditions: None (handles NULL/empty gracefully)
 * Postconditions: Returns 0 or 1 only
 */
static inline int get_processor_id_for_symbol(const char* symbol) {
    /* Handle NULL/empty - default to processor 0 */
    if (symbol == NULL || symbol[0] == '\0') {
        return PROCESSOR_ID_A_TO_M;
    }

    /* Get first character */
    unsigned int c = (unsigned char)symbol[0];

    /*
     * Branchless uppercase conversion:
     * If 'a' <= c <= 'z', subtract 32 to get uppercase.
     * Using unsigned arithmetic to avoid branches.
     *
     * is_lower = 1 if lowercase, 0 otherwise
     * Computed as: (c >= 'a') & (c <= 'z')
     */
    unsigned int is_lower = (c >= 'a') & (c <= 'z');
    c -= is_lower * ('a' - 'A');

    /*
     * Branchless N-Z detection:
     * is_n_to_z = 1 if c in ['N'..'Z'], 0 otherwise
     *
     * For non-alphabetic characters, this returns 0 (processor A-M),
     * which is the safe default.
     */
    unsigned int is_upper_n_to_z = (c >= 'N') & (c <= 'Z');

    /* Result is 0 or 1, matching processor IDs exactly */
    int result = (int)is_upper_n_to_z;

    /* Postcondition: result is valid processor ID */
    assert((result == 0 || result == 1) && "Invalid processor ID");

    return result;
}

/**
 * Get processor ID using only the first character (for binary protocol)
 *
 * Avoids needing the full symbol string - useful when only first
 * character is available or when parsing binary messages.
 *
 * @param first The first character of the symbol
 * @return 0 for A-M (or invalid), 1 for N-Z
 *
 * Preconditions: None (handles '\0' gracefully)
 * Postconditions: Returns 0 or 1 only
 */
static inline int get_processor_id_for_char(char first) {
    /* Handle empty */
    if (first == '\0') {
        return PROCESSOR_ID_A_TO_M;
    }

    unsigned int c = (unsigned char)first;

    /* Branchless uppercase conversion */
    unsigned int is_lower = (c >= 'a') & (c <= 'z');
    c -= is_lower * ('a' - 'A');

    /* Branchless N-Z detection */
    unsigned int is_upper_n_to_z = (c >= 'N') & (c <= 'Z');

    int result = (int)is_upper_n_to_z;

    assert((result == 0 || result == 1) && "Invalid processor ID");

    return result;
}

/* ============================================================================
 * Validation Functions
 * ============================================================================ */

/**
 * Check if a symbol string is valid (non-empty, starts with letter)
 *
 * @param symbol The symbol to validate
 * @return true if symbol is valid for routing
 *
 * Preconditions: None (handles NULL gracefully)
 */
static inline bool symbol_is_valid(const char* symbol) {
    if (symbol == NULL || symbol[0] == '\0') {
        return false;
    }

    unsigned int c = (unsigned char)symbol[0];

    /*
     * Check if alphabetic (A-Z or a-z) using branchless logic:
     * is_upper = (c >= 'A') & (c <= 'Z')
     * is_lower = (c >= 'a') & (c <= 'z')
     * is_alpha = is_upper | is_lower
     */
    unsigned int is_upper = (c >= 'A') & (c <= 'Z');
    unsigned int is_lower = (c >= 'a') & (c <= 'z');

    return (is_upper | is_lower) != 0;
}

/**
 * Check if a character is valid as first character of symbol
 *
 * @param first The character to validate
 * @return true if character is alphabetic
 */
static inline bool symbol_char_is_valid(char first) {
    if (first == '\0') {
        return false;
    }

    unsigned int c = (unsigned char)first;

    unsigned int is_upper = (c >= 'A') & (c <= 'Z');
    unsigned int is_lower = (c >= 'a') & (c <= 'z');

    return (is_upper | is_lower) != 0;
}

/* ============================================================================
 * Debug/Display Functions
 * ============================================================================ */

/**
 * Get human-readable name for processor
 *
 * @param processor_id The processor ID (0 or 1)
 * @return String name ("A-M", "N-Z", or "Unknown")
 */
static inline const char* get_processor_name(int processor_id) {
    /*
     * Using array lookup instead of switch for branchless access.
     * Index 2 is the fallback for invalid IDs.
     */
    static const char* const names[3] = { "A-M", "N-Z", "Unknown" };

    /* Bounds check - any invalid ID maps to index 2 */
    if (processor_id < 0 || processor_id >= NUM_PROCESSORS) {
        return names[2];
    }

    return names[processor_id];
}

/**
 * Get processor ID with validation (debug builds)
 *
 * Same as get_processor_id_for_symbol but with additional assertions
 * for debugging routing issues.
 *
 * @param symbol The trading symbol
 * @return Processor ID (0 or 1)
 */
static inline int get_processor_id_for_symbol_debug(const char* symbol) {
    int result = get_processor_id_for_symbol(symbol);

    /* Additional debug assertions */
    assert(result >= 0 && result < NUM_PROCESSORS && "Processor ID out of range");

    /* Verify consistency: same symbol always routes to same processor */
    assert(get_processor_id_for_symbol(symbol) == result && "Routing not deterministic");

    return result;
}

#ifdef __cplusplus
}
#endif

#endif /* SYMBOL_ROUTER_H */

#ifndef PLATFORM_TIMESTAMPS_H
#define PLATFORM_TIMESTAMPS_H

/**
 * Platform Timestamps - Cross-platform high-resolution timestamps
 *
 * Provides nanosecond-resolution timestamps using the best available
 * mechanism for each platform:
 * - x86-64 Linux/macOS: RDTSCP (serializing, ~5 cycles)
 * - ARM64: clock_gettime (~25ns)
 * - Fallback: clock_gettime
 */

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Timestamp Functions
 * ============================================================================ */

/**
 * Get high-resolution timestamp in nanoseconds
 *
 * Uses CLOCK_MONOTONIC for consistent, non-jumping time.
 */
static inline uint64_t get_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * Get timestamp using RDTSC (x86-64 only)
 *
 * Returns raw CPU cycle count. Use for relative timing only.
 * Note: Uses RDTSCP which is serializing (waits for prior instructions).
 */
#if defined(__x86_64__) || defined(_M_X64)
static inline uint64_t get_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__ (
        "rdtscp"
        : "=a" (lo), "=d" (hi)
        :
        : "rcx"
    );
    return ((uint64_t)hi << 32) | lo;
}
#else
/* Fallback for non-x86 platforms */
static inline uint64_t get_rdtsc(void) {
    return get_timestamp();
}
#endif

/**
 * Get timestamp in microseconds
 */
static inline uint64_t get_timestamp_us(void) {
    return get_timestamp() / 1000ULL;
}

/**
 * Get timestamp in milliseconds
 */
static inline uint64_t get_timestamp_ms(void) {
    return get_timestamp() / 1000000ULL;
}

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_TIMESTAMPS_H */

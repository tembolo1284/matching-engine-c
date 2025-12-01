/**
 * scenarios.h - Test scenarios for matching engine client
 *
 * Provides pre-defined test scenarios ported from the Zig implementation:
 *   - Basic scenarios (1-3): Simple orders, matching, cancellation
 *   - Stress tests (10-15): Throttled high-volume order submission
 *   - Matching stress (20-23): Generate trades at scale
 *   - Multi-symbol stress (30-32): Test dual-processor routing
 *   - Burst mode (40-41): Unthrottled maximum speed (danger!)
 */

#ifndef CLIENT_SCENARIOS_H
#define CLIENT_SCENARIOS_H

#include "client/engine_client.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Scenario Information
 * ============================================================ */

/**
 * Scenario category
 */
typedef enum {
    SCENARIO_CAT_BASIC = 0,
    SCENARIO_CAT_STRESS,
    SCENARIO_CAT_MATCHING,
    SCENARIO_CAT_MULTI_SYMBOL,
    SCENARIO_CAT_BURST
} scenario_category_t;

/**
 * Scenario descriptor
 */
typedef struct {
    int                 id;
    const char*         name;
    const char*         description;
    scenario_category_t category;
    uint32_t            order_count;        /* Approximate orders sent */
    bool                requires_burst;     /* Needs --danger-burst flag */
} scenario_info_t;

/* ============================================================
 * Scenario Results
 * ============================================================ */

/**
 * Results from running a scenario
 */
typedef struct {
    /* Counts */
    uint32_t            orders_sent;
    uint32_t            orders_failed;
    uint32_t            responses_received;
    uint32_t            trades_executed;
    
    /* Timing */
    uint64_t            start_time_ns;
    uint64_t            end_time_ns;
    uint64_t            total_time_ns;
    
    /* Latency (nanoseconds) */
    uint64_t            min_latency_ns;
    uint64_t            avg_latency_ns;
    uint64_t            max_latency_ns;
    
    /* Throughput */
    double              orders_per_sec;
    double              messages_per_sec;
    
    /* Processor distribution (for multi-symbol) */
    uint32_t            proc0_orders;
    uint32_t            proc1_orders;
    
} scenario_result_t;

/* ============================================================
 * Scenario API
 * ============================================================ */

/**
 * Run a scenario by ID
 * 
 * @param client        Connected engine client
 * @param scenario_id   Scenario number (1, 2, 3, 10-15, 20-23, 30-32, 40-41)
 * @param danger_burst  Allow burst mode scenarios (40-41)
 * @param result        Output: scenario results (may be NULL)
 * @return              true on success, false on error or unknown scenario
 */
bool scenario_run(engine_client_t* client,
                  int scenario_id,
                  bool danger_burst,
                  scenario_result_t* result);

/**
 * Get scenario information
 * 
 * @param scenario_id   Scenario number
 * @return              Pointer to scenario info, or NULL if unknown
 */
const scenario_info_t* scenario_get_info(int scenario_id);

/**
 * Check if scenario ID is valid
 */
bool scenario_is_valid(int scenario_id);

/**
 * Check if scenario requires burst mode
 */
bool scenario_requires_burst(int scenario_id);

/**
 * Print list of available scenarios
 */
void scenario_print_list(void);

/**
 * Print scenario results
 */
void scenario_print_result(const scenario_result_t* result);

/* ============================================================
 * Individual Scenario Functions
 * ============================================================ */

/* Basic scenarios */
bool scenario_simple_orders(engine_client_t* client, scenario_result_t* result);
bool scenario_matching_trade(engine_client_t* client, scenario_result_t* result);
bool scenario_cancel_order(engine_client_t* client, scenario_result_t* result);

/* Stress tests */
bool scenario_stress_test(engine_client_t* client, uint32_t count, 
                          bool throttled, scenario_result_t* result);

/* Matching stress */
bool scenario_matching_stress(engine_client_t* client, uint32_t pairs,
                              scenario_result_t* result);

/* Multi-symbol stress */
bool scenario_multi_symbol_stress(engine_client_t* client, uint32_t count,
                                  scenario_result_t* result);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_SCENARIOS_H */

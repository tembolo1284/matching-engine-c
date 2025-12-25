/**
 * scenarios.c - Test scenarios implementation
 *
 * Focused on matching throughput - the real strength of a matching engine.
 * Large one-sided stress tests removed since they just fill the pool.
 * Small stress tests (up to 100K) kept for quick validation.
 */
#include "client/scenarios.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ============================================================
 * Scenario Registry
 * ============================================================ */

static const scenario_info_t SCENARIOS[] = {
    /* Basic scenarios - for correctness testing */
    { 1,  "simple-orders",    "Simple orders (no match)",              SCENARIO_CAT_BASIC,        3,          false },
    { 2,  "matching-trade",   "Matching trade execution",              SCENARIO_CAT_BASIC,        2,          false },
    { 3,  "cancel-order",     "Cancel order",                          SCENARIO_CAT_BASIC,        2,          false },

    /* Small stress tests (non-matching) - for quick validation */
    { 10, "stress-1k",        "Stress: 1K orders (no match)",          SCENARIO_CAT_STRESS,       1000,       false },
    { 11, "stress-10k",       "Stress: 10K orders (no match)",         SCENARIO_CAT_STRESS,       10000,      false },
    { 12, "stress-100k",      "Stress: 100K orders (no match)",        SCENARIO_CAT_STRESS,       100000,     false },

    /* Matching stress - single symbol (sustainable throughput) */
    { 20, "match-1k",         "Matching: 1K pairs (2K orders)",        SCENARIO_CAT_MATCHING,     2000,       false },
    { 21, "match-10k",        "Matching: 10K pairs (20K orders)",      SCENARIO_CAT_MATCHING,     20000,      false },
    { 22, "match-100k",       "Matching: 100K pairs (200K orders)",    SCENARIO_CAT_MATCHING,     200000,     false },
    { 23, "match-1m",         "Matching: 1M pairs (2M orders)",        SCENARIO_CAT_MATCHING,     2000000,    false },
    { 24, "match-10m",        "Matching: 10M pairs (20M orders)",      SCENARIO_CAT_MATCHING,     20000000,   false },
    { 25, "match-50m",        "Matching: 50M pairs (100M orders)",     SCENARIO_CAT_MATCHING,     100000000,  false },

    /* Multi-symbol matching - dual processor (ultimate throughput test) */
    { 26, "match-multi-250m", "Dual-Proc: 250M pairs (500M orders)",   SCENARIO_CAT_MATCHING,     500000000,  false },
    { 27, "match-multi-500m", "Dual-Proc: 500M pairs (1B orders)",     SCENARIO_CAT_MATCHING,     1000000000, false },

    { 0, NULL, NULL, 0, 0, false }  /* Sentinel */
};

static const int NUM_SCENARIOS = sizeof(SCENARIOS) / sizeof(SCENARIOS[0]) - 1;

/* Symbols for dual-processor matching tests */
static const char* DUAL_PROC_SYMBOLS[] = {
    /* Processor 0 (A-M) - 5 symbols */
    "AAPL", "IBM", "GOOGL", "META", "MSFT",
    /* Processor 1 (N-Z) - 5 symbols */
    "NVDA", "TSLA", "UBER", "SNAP", "ZM"
};
static const int NUM_DUAL_PROC_SYMBOLS = 10;

/* ============================================================
 * Helper Functions
 * ============================================================ */

static void init_result(scenario_result_t* result) {
    if (result) {
        memset(result, 0, sizeof(*result));
    }
}

static void finalize_result(scenario_result_t* result, engine_client_t* client) {
    if (!result) return;

    result->end_time_ns = engine_client_now_ns();
    result->total_time_ns = result->end_time_ns - result->start_time_ns;

    /* Copy latency stats from client */
    result->min_latency_ns = engine_client_get_min_latency_ns(client);
    result->avg_latency_ns = engine_client_get_avg_latency_ns(client);
    result->max_latency_ns = engine_client_get_max_latency_ns(client);

    /* Calculate throughput */
    if (result->total_time_ns > 0) {
        double seconds = (double)result->total_time_ns / 1e9;
        result->orders_per_sec = (double)result->orders_sent / seconds;
        result->messages_per_sec = (double)(result->orders_sent + result->responses_received) / seconds;
    }
}

static void sleep_ms(int ms) {
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

/**
 * Simple drain for basic scenarios - just wait for silence.
 */
static void drain_responses(engine_client_t* client, int initial_delay_ms) {
    sleep_ms(initial_delay_ms);

    int empty_count = 0;

    while (empty_count < 5) {
        int count = engine_client_recv_all(client, 50);
        if (count == 0) {
            empty_count++;
            sleep_ms(20);
        } else {
            empty_count = 0;
        }
    }
}

/**
 * Aggressive drain - keep receiving until we hit target trades or stall.
 * Used at end of matching scenarios to ensure all trades received.
 */
static void drain_until_complete(engine_client_t* client,
                                  scenario_result_t* result,
                                  uint32_t expected_trades) {
    uint64_t start_ns = engine_client_now_ns();
    uint64_t timeout_ns = 30000000000ULL;  /* 30 second hard timeout */
    
    uint32_t last_trade_count = result->trades_executed;
    int stall_iterations = 0;
    
    while (result->trades_executed < expected_trades) {
        uint64_t elapsed = engine_client_now_ns() - start_ns;
        if (elapsed > timeout_ns) {
            printf("  [timeout]\n");
            break;
        }
        
        int count = engine_client_recv_all(client, 50);
        
        if (count > 0) {
            stall_iterations = 0;
        } else {
            if (result->trades_executed == last_trade_count) {
                stall_iterations++;
                if (stall_iterations > 50) {  /* ~2.5 seconds stall */
                    break;
                }
            } else {
                last_trade_count = result->trades_executed;
                stall_iterations = 0;
            }
            sleep_ms(20);
        }
    }
}

/**
 * Response callback that counts trades
 */
typedef struct {
    scenario_result_t* result;
    bool verbose;
} callback_context_t;

static void response_counter(const output_msg_t* msg, void* user_data) {
    callback_context_t* ctx = (callback_context_t*)user_data;

    if (ctx->result) {
        ctx->result->responses_received++;
        if (msg->type == OUTPUT_MSG_TRADE) {
            ctx->result->trades_executed++;
        }
    }

    if (ctx->verbose) {
        switch (msg->type) {
            case OUTPUT_MSG_ACK:
                printf("[RECV] A, %s, %u, %u\n",
                       msg->data.ack.symbol,
                       msg->data.ack.user_id,
                       msg->data.ack.user_order_id);
                break;
            case OUTPUT_MSG_CANCEL_ACK:
                printf("[RECV] C, %s, %u, %u\n",
                       msg->data.cancel_ack.symbol,
                       msg->data.cancel_ack.user_id,
                       msg->data.cancel_ack.user_order_id);
                break;
            case OUTPUT_MSG_TRADE:
                printf("[RECV] T, %s, %u, %u, %u, %u, %u, %u\n",
                       msg->data.trade.symbol,
                       msg->data.trade.user_id_buy,
                       msg->data.trade.user_order_id_buy,
                       msg->data.trade.user_id_sell,
                       msg->data.trade.user_order_id_sell,
                       msg->data.trade.price,
                       msg->data.trade.quantity);
                break;
            case OUTPUT_MSG_TOP_OF_BOOK:
                if (msg->data.top_of_book.price == 0 &&
                    msg->data.top_of_book.total_quantity == 0) {
                    printf("[RECV] B, %s, %c, -, -\n",
                           msg->data.top_of_book.symbol,
                           (char)msg->data.top_of_book.side);
                } else {
                    printf("[RECV] B, %s, %c, %u, %u\n",
                           msg->data.top_of_book.symbol,
                           (char)msg->data.top_of_book.side,
                           msg->data.top_of_book.price,
                           msg->data.top_of_book.total_quantity);
                }
                break;
        }
    }
}

/* ============================================================
 * Scenario Registry Functions
 * ============================================================ */

const scenario_info_t* scenario_get_info(int scenario_id) {
    for (int i = 0; i < NUM_SCENARIOS; i++) {
        if (SCENARIOS[i].id == scenario_id) {
            return &SCENARIOS[i];
        }
    }
    return NULL;
}

bool scenario_is_valid(int scenario_id) {
    return scenario_get_info(scenario_id) != NULL;
}

bool scenario_requires_burst(int scenario_id) {
    const scenario_info_t* info = scenario_get_info(scenario_id);
    return info && info->requires_burst;
}

void scenario_print_list(void) {
    printf("Available scenarios:\n");
    printf("\n");

    printf("Basic (correctness testing):\n");
    for (int i = 0; SCENARIOS[i].id != 0; i++) {
        if (SCENARIOS[i].category == SCENARIO_CAT_BASIC) {
            printf("  %-3d - %s\n", SCENARIOS[i].id, SCENARIOS[i].description);
        }
    }

    printf("\nStress Tests (non-matching, up to 100K - quick validation):\n");
    for (int i = 0; SCENARIOS[i].id != 0; i++) {
        if (SCENARIOS[i].category == SCENARIO_CAT_STRESS) {
            printf("  %-3d - %s\n", SCENARIOS[i].id, SCENARIOS[i].description);
        }
    }

    printf("\nMatching Stress (sustainable throughput - THE REAL TEST):\n");
    for (int i = 0; SCENARIOS[i].id != 0; i++) {
        if (SCENARIOS[i].category == SCENARIO_CAT_MATCHING) {
            printf("  %-3d - %s\n", SCENARIOS[i].id, SCENARIOS[i].description);
        }
    }
}

void scenario_print_result(const scenario_result_t* result) {
    printf("\n");
    printf("=== Scenario Results ===\n");
    printf("\n");
    printf("Orders:\n");
    printf("  Sent:              %u\n", result->orders_sent);
    printf("  Failed:            %u\n", result->orders_failed);
    printf("  Responses:         %u\n", result->responses_received);
    printf("  Trades:            %u\n", result->trades_executed);
    printf("\n");

    /* Format time nicely */
    if (result->total_time_ns >= 60000000000ULL) {
        double minutes = (double)result->total_time_ns / 6e10;
        printf("Time:                %.2f min\n", minutes);
    } else if (result->total_time_ns >= 1000000000ULL) {
        printf("Time:                %.3f sec\n",
               (double)result->total_time_ns / 1e9);
    } else {
        printf("Time:                %.3f ms\n",
               (double)result->total_time_ns / 1e6);
    }
    printf("\n");

    printf("Throughput:\n");
    if (result->orders_per_sec >= 1000000) {
        printf("  Orders/sec:        %.2fM\n", result->orders_per_sec / 1e6);
    } else if (result->orders_per_sec >= 1000) {
        printf("  Orders/sec:        %.2fK\n", result->orders_per_sec / 1e3);
    } else {
        printf("  Orders/sec:        %.0f\n", result->orders_per_sec);
    }
    printf("\n");

    if (result->min_latency_ns > 0) {
        printf("Latency (round-trip):\n");
        printf("  Min:               %.3f us\n", (double)result->min_latency_ns / 1e3);
        printf("  Avg:               %.3f us\n", (double)result->avg_latency_ns / 1e3);
        printf("  Max:               %.3f us\n", (double)result->max_latency_ns / 1e3);
        printf("\n");
    }

    if (result->proc0_orders > 0 || result->proc1_orders > 0) {
        printf("Processor Distribution:\n");
        printf("  Processor 0 (A-M): %u orders (%.1f%%)\n",
               result->proc0_orders,
               (result->proc0_orders + result->proc1_orders) > 0
                   ? (100.0 * result->proc0_orders / (result->proc0_orders + result->proc1_orders))
                   : 0.0);
        printf("  Processor 1 (N-Z): %u orders (%.1f%%)\n",
               result->proc1_orders,
               (result->proc0_orders + result->proc1_orders) > 0
                   ? (100.0 * result->proc1_orders / (result->proc0_orders + result->proc1_orders))
                   : 0.0);
        printf("\n");
    }
}

/* ============================================================
 * Basic Scenarios
 * ============================================================ */

bool scenario_simple_orders(engine_client_t* client, scenario_result_t* result) {
    printf("=== Scenario 1: Simple Orders ===\n\n");

    init_result(result);
    if (result) result->start_time_ns = engine_client_now_ns();

    callback_context_t ctx = { .result = result, .verbose = true };
    engine_client_set_response_callback(client, response_counter, &ctx);

    printf("Sending: BUY IBM 50@100\n");
    uint32_t oid = engine_client_send_order(client, "IBM", 100, 50, SIDE_BUY, 0);
    if (oid == 0) {
        if (result) result->orders_failed++;
    } else {
        if (result) result->orders_sent++;
    }

    drain_responses(client, 150);

    printf("\nSending: SELL IBM 50@105\n");
    oid = engine_client_send_order(client, "IBM", 105, 50, SIDE_SELL, 0);
    if (oid == 0) {
        if (result) result->orders_failed++;
    } else {
        if (result) result->orders_sent++;
    }

    drain_responses(client, 150);

    printf("\nSending: FLUSH\n");
    engine_client_send_flush(client);
    if (result) result->orders_sent++;

    drain_responses(client, 250);

    finalize_result(result, client);
    return true;
}

bool scenario_matching_trade(engine_client_t* client, scenario_result_t* result) {
    printf("=== Scenario 2: Matching Trade ===\n\n");

    init_result(result);
    if (result) result->start_time_ns = engine_client_now_ns();

    callback_context_t ctx = { .result = result, .verbose = true };
    engine_client_set_response_callback(client, response_counter, &ctx);

    printf("Sending: BUY IBM 50@100\n");
    uint32_t oid = engine_client_send_order(client, "IBM", 100, 50, SIDE_BUY, 0);
    if (oid > 0 && result) result->orders_sent++;

    drain_responses(client, 150);

    printf("\nSending: SELL IBM 50@100 (should match!)\n");
    oid = engine_client_send_order(client, "IBM", 100, 50, SIDE_SELL, 0);
    if (oid > 0 && result) result->orders_sent++;

    drain_responses(client, 200);

    finalize_result(result, client);
    return true;
}

bool scenario_cancel_order(engine_client_t* client, scenario_result_t* result) {
    printf("=== Scenario 3: Cancel Order ===\n\n");

    init_result(result);
    if (result) result->start_time_ns = engine_client_now_ns();

    callback_context_t ctx = { .result = result, .verbose = true };
    engine_client_set_response_callback(client, response_counter, &ctx);

    printf("Sending: BUY IBM 50@100\n");
    uint32_t oid = engine_client_send_order(client, "IBM", 100, 50, SIDE_BUY, 0);
    if (oid > 0 && result) result->orders_sent++;

    drain_responses(client, 150);

    printf("\nSending: CANCEL order %u\n", oid);
    engine_client_send_cancel(client, oid);

    drain_responses(client, 150);

    finalize_result(result, client);
    return true;
}

/* ============================================================
 * Small Stress Test (Non-Matching, up to 100K)
 * ============================================================ */

bool scenario_stress_test(engine_client_t* client, uint32_t count,
                          bool danger_burst, scenario_result_t* result) {
    (void)danger_burst;

    printf("=== Stress Test: %u Orders (non-matching) ===\n\n", count);

    init_result(result);
    if (result) result->start_time_ns = engine_client_now_ns();

    callback_context_t ctx = { .result = result, .verbose = false };
    engine_client_set_response_callback(client, response_counter, &ctx);

    /* Flush first */
    engine_client_send_flush(client);
    drain_responses(client, 100);
    if (result) result->responses_received = 0;

    /* Progress tracking */
    uint32_t progress_interval = count / 20;
    if (progress_interval == 0) progress_interval = 1;
    uint32_t last_progress = 0;
    uint64_t start_time = engine_client_now_ns();

    for (uint32_t i = 0; i < count; i++) {
        uint32_t price = 100 + (i % 100);
        uint32_t oid = engine_client_send_order(client, "IBM", price, 10, SIDE_BUY, 0);
        if (oid > 0) {
            if (result) result->orders_sent++;
        } else {
            if (result) result->orders_failed++;
        }

        /* Interleave receives */
        engine_client_recv_all(client, 0);

        if (i > 0 && i / progress_interval > last_progress) {
            last_progress = i / progress_interval;
            uint32_t pct = (i * 100) / count;
            uint64_t elapsed_ns = engine_client_now_ns() - start_time;
            uint64_t elapsed_ms = elapsed_ns / 1000000;
            uint64_t rate = (elapsed_ms > 0) ? ((uint64_t)i * 1000 / elapsed_ms) : 0;
            printf("  %u%% (%u orders, %lu ms, %lu orders/sec)\n",
                   pct, i, (unsigned long)elapsed_ms, (unsigned long)rate);
        }
    }

    printf("\nSending FLUSH to clear book...\n");
    engine_client_send_flush(client);
    
    /* Simple drain for stress tests */
    int empty_count = 0;
    while (empty_count < 30) {
        int count_recv = engine_client_recv_all(client, 50);
        if (count_recv == 0) {
            empty_count++;
            sleep_ms(20);
        } else {
            empty_count = 0;
        }
    }

    finalize_result(result, client);
    scenario_print_result(result);
    return true;
}

/* ============================================================
 * Matching Stress (Single Symbol) - PACED SENDING
 * ============================================================ */

bool scenario_matching_stress(engine_client_t* client, uint32_t pairs,
                              scenario_result_t* result) {
    printf("=== Matching Stress Test: %u Trade Pairs ===\n\n", pairs);
    printf("Sending %u buy/sell pairs (should generate %u trades)...\n\n", pairs, pairs);

    init_result(result);
    if (result) result->start_time_ns = engine_client_now_ns();

    callback_context_t ctx = { .result = result, .verbose = false };
    engine_client_set_response_callback(client, response_counter, &ctx);

    /* Flush first */
    engine_client_send_flush(client);
    drain_responses(client, 200);
    if (result) result->responses_received = 0;

    /* Progress tracking */
    uint32_t progress_interval = pairs / 20;
    if (progress_interval == 0) progress_interval = 1;
    uint32_t last_progress = 0;
    uint64_t start_time = engine_client_now_ns();
    
    /* Pacing: drain aggressively every N pairs to prevent buffer overflow */
    uint32_t drain_interval = 1000;  /* Drain every 1000 pairs */
    if (pairs >= 1000000) drain_interval = 10000;
    else if (pairs >= 100000) drain_interval = 5000;

    for (uint32_t i = 0; i < pairs; i++) {
        uint32_t price = 100 + (i % 50);

        /* Send buy */
        uint32_t buy_oid = engine_client_send_order(client, "IBM", price, 10, SIDE_BUY, 0);
        if (buy_oid > 0 && result) result->orders_sent++;

        /* Quick non-blocking receive */
        engine_client_recv_all(client, 0);

        /* Send matching sell */
        uint32_t sell_oid = engine_client_send_order(client, "IBM", price, 10, SIDE_SELL, 0);
        if (sell_oid > 0 && result) result->orders_sent++;

        /* Quick non-blocking receive */
        engine_client_recv_all(client, 0);

        /* Periodic aggressive drain to keep up with server */
        if ((i + 1) % drain_interval == 0) {
            /* Drain until we've caught up somewhat */
            int drain_rounds = 0;
            while (drain_rounds < 20) {
                int count = engine_client_recv_all(client, 10);
                if (count == 0) break;
                drain_rounds++;
            }
        }

        /* Progress indicator */
        if (i > 0 && i / progress_interval > last_progress) {
            last_progress = i / progress_interval;
            uint32_t pct = (uint32_t)(((uint64_t)i * 100ULL) / pairs);
            uint64_t elapsed_ns = engine_client_now_ns() - start_time;
            uint64_t elapsed_ms = elapsed_ns / 1000000;
            uint64_t orders_sent = (uint64_t)i * 2;
            uint64_t rate = (elapsed_ms > 0) ? (orders_sent * 1000 / elapsed_ms) : 0;
            printf("  %u%% (%u pairs, %lu ms, %lu orders/sec, %u trades so far)\n",
                   pct, i, (unsigned long)elapsed_ms, (unsigned long)rate,
                   result->trades_executed);
        }
    }

    /* Final drain - keep going until all trades received or stalled */
    printf("\nDraining remaining responses...\n");
    printf("  [sent %u pairs, received %u trades so far]\n", 
           pairs, result->trades_executed);
    
    drain_until_complete(client, result, pairs);

    finalize_result(result, client);
    scenario_print_result(result);
    
    /* Validation */
    if (result->trades_executed != pairs) {
        printf("⚠ WARNING: Expected %u trades, got %u (%.1f%%)\n\n", 
               pairs, result->trades_executed,
               (100.0 * result->trades_executed) / pairs);
    } else {
        printf("✓ All %u trades executed successfully!\n\n", pairs);
    }
    
    return true;
}

/* ============================================================
 * Multi-Symbol Matching Stress (Dual Processor) - PACED SENDING
 * ============================================================ */

bool scenario_multi_symbol_matching_stress(engine_client_t* client, uint32_t pairs,
                                           scenario_result_t* result) {
    printf("============================================================\n");
    printf("  DUAL-PROCESSOR MATCHING STRESS TEST\n");
    printf("============================================================\n\n");
    printf("Trade Pairs:     %u\n", pairs);
    printf("Total Orders:    %u\n", pairs * 2);
    printf("Expected Trades: %u\n", pairs);
    printf("Symbols:         10 (5 per processor)\n");
    printf("  Processor 0:   AAPL, IBM, GOOGL, META, MSFT\n");
    printf("  Processor 1:   NVDA, TSLA, UBER, SNAP, ZM\n");
    printf("============================================================\n\n");

    init_result(result);
    if (result) result->start_time_ns = engine_client_now_ns();

    callback_context_t ctx = { .result = result, .verbose = false };
    engine_client_set_response_callback(client, response_counter, &ctx);

    printf("Flushing existing orders...\n");
    engine_client_send_flush(client);
    drain_responses(client, 500);
    if (result) result->responses_received = 0;

    printf("Starting benchmark...\n\n");

    uint32_t progress_interval = pairs / 20;
    if (progress_interval == 0) progress_interval = 1;
    uint32_t last_progress = 0;

    uint32_t proc0_count = 0;
    uint32_t proc1_count = 0;

    uint64_t start_time = engine_client_now_ns();
    
    /* Pacing interval */
    uint32_t drain_interval = 10000;
    if (pairs >= 100000000) drain_interval = 50000;
    else if (pairs >= 10000000) drain_interval = 25000;

    for (uint32_t i = 0; i < pairs; i++) {
        int symbol_idx = i % NUM_DUAL_PROC_SYMBOLS;
        const char* symbol = DUAL_PROC_SYMBOLS[symbol_idx];
        uint32_t price = 100 + (i % 50);

        uint32_t buy_oid = engine_client_send_order(client, symbol, price, 10, SIDE_BUY, 0);
        if (buy_oid > 0) {
            if (result) result->orders_sent++;
            if (symbol_idx < 5) proc0_count++;
            else proc1_count++;
        }

        engine_client_recv_all(client, 0);

        uint32_t sell_oid = engine_client_send_order(client, symbol, price, 10, SIDE_SELL, 0);
        if (sell_oid > 0) {
            if (result) result->orders_sent++;
            if (symbol_idx < 5) proc0_count++;
            else proc1_count++;
        }

        engine_client_recv_all(client, 0);

        /* Periodic aggressive drain */
        if ((i + 1) % drain_interval == 0) {
            int drain_rounds = 0;
            while (drain_rounds < 30) {
                int count = engine_client_recv_all(client, 10);
                if (count == 0) break;
                drain_rounds++;
            }
        }

        if (i > 0 && i / progress_interval > last_progress) {
            last_progress = i / progress_interval;
            uint32_t pct = (uint32_t)(((uint64_t)i * 100ULL) / pairs);
            uint64_t elapsed_ns = engine_client_now_ns() - start_time;
            uint64_t elapsed_ms = elapsed_ns / 1000000;
            uint64_t elapsed_sec = elapsed_ms / 1000;
            uint64_t orders_sent = (uint64_t)i * 2;
            uint64_t rate = (elapsed_ms > 0) ? (orders_sent * 1000 / elapsed_ms) : 0;

            if (elapsed_sec >= 60) {
                uint64_t mins = elapsed_sec / 60;
                uint64_t secs = elapsed_sec % 60;
                printf("  %3u%% | %10u pairs | %2lu:%02lu | %lu/s | %u trades\n",
                       pct, i, (unsigned long)mins, (unsigned long)secs, 
                       (unsigned long)rate, result->trades_executed);
            } else {
                printf("  %3u%% | %10u pairs | %5lums | %lu/s | %u trades\n",
                       pct, i, (unsigned long)elapsed_ms, 
                       (unsigned long)rate, result->trades_executed);
            }
        }
    }

    if (result) {
        result->proc0_orders = proc0_count;
        result->proc1_orders = proc1_count;
    }

    uint64_t send_elapsed_ns = engine_client_now_ns() - start_time;
    double send_elapsed_sec = (double)send_elapsed_ns / 1e9;

    printf("\n");
    printf("============================================================\n");
    printf("  SEND COMPLETE\n");
    printf("============================================================\n");
    printf("Orders sent:     %u\n", result ? result->orders_sent : 0);
    printf("Send time:       %.2f sec\n", send_elapsed_sec);
    printf("Send rate:       %.2fM orders/sec\n",
           (result ? result->orders_sent : 0) / send_elapsed_sec / 1e6);
    printf("Trades so far:   %u\n", result->trades_executed);
    printf("============================================================\n\n");

    printf("Draining remaining responses...\n");
    drain_until_complete(client, result, pairs);

    finalize_result(result, client);
    scenario_print_result(result);
    
    if (result->trades_executed != pairs) {
        printf("⚠ WARNING: Expected %u trades, got %u (%.1f%%)\n\n", 
               pairs, result->trades_executed,
               (100.0 * result->trades_executed) / pairs);
    } else {
        printf("✓ All %u trades executed successfully!\n\n", pairs);
    }
    
    return true;
}

/* ============================================================
 * Main Scenario Runner
 * ============================================================ */

bool scenario_run(engine_client_t* client,
                  int scenario_id,
                  bool danger_burst,
                  scenario_result_t* result) {

    const scenario_info_t* info = scenario_get_info(scenario_id);
    if (!info) {
        fprintf(stderr, "Unknown scenario: %d\n\n", scenario_id);
        scenario_print_list();
        return false;
    }

    if (info->requires_burst && !danger_burst) {
        fprintf(stderr, "Scenario %d requires --danger-burst flag!\n", scenario_id);
        fprintf(stderr, "This scenario sends orders without throttling and may\n");
        fprintf(stderr, "cause server buffer overflows or parse errors.\n");
        return false;
    }

    engine_client_reset_stats(client);
    engine_client_reset_order_id(client, 1);

    switch (scenario_id) {
        case 1:  return scenario_simple_orders(client, result);
        case 2:  return scenario_matching_trade(client, result);
        case 3:  return scenario_cancel_order(client, result);

        case 10: return scenario_stress_test(client, 1000, danger_burst, result);
        case 11: return scenario_stress_test(client, 10000, danger_burst, result);
        case 12: return scenario_stress_test(client, 100000, danger_burst, result);

        case 20: return scenario_matching_stress(client, 1000, result);
        case 21: return scenario_matching_stress(client, 10000, result);
        case 22: return scenario_matching_stress(client, 100000, result);
        case 23: return scenario_matching_stress(client, 1000000, result);
        case 24: return scenario_matching_stress(client, 10000000, result);
        case 25: return scenario_matching_stress(client, 50000000, result);

        case 26: return scenario_multi_symbol_matching_stress(client, 250000000, result);
        case 27: return scenario_multi_symbol_matching_stress(client, 500000000, result);

        default:
            fprintf(stderr, "Scenario %d not implemented\n", scenario_id);
            return false;
    }
}

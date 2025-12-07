Got it! Keep the small non-matching tests (1K, 10K, 100K) but remove the large ones (1M+) that just fill the pool. Here are the complete files:
1. Complete tools/client/scenarios.c:
c/**
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
        /* More than 1 minute */
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

    /* Buy order */
    printf("Sending: BUY IBM 50@100\n");
    uint32_t oid = engine_client_send_order(client, "IBM", 100, 50, SIDE_BUY, 0);
    if (oid == 0) {
        if (result) result->orders_failed++;
    } else {
        if (result) result->orders_sent++;
    }
    sleep_ms(200);
    engine_client_recv_all(client, 200);

    /* Sell order at different price (no match) */
    printf("\nSending: SELL IBM 50@105\n");
    oid = engine_client_send_order(client, "IBM", 105, 50, SIDE_SELL, 0);
    if (oid == 0) {
        if (result) result->orders_failed++;
    } else {
        if (result) result->orders_sent++;
    }
    sleep_ms(200);
    engine_client_recv_all(client, 200);

    /* Flush */
    printf("\nSending: FLUSH\n");
    engine_client_send_flush(client);
    if (result) result->orders_sent++;

    sleep_ms(250);
    engine_client_recv_all(client, 250);

    for (int i = 0; i < 5; i++) {
        sleep_ms(200);
        engine_client_recv_all(client, 200);
    }

    finalize_result(result, client);
    return true;
}

bool scenario_matching_trade(engine_client_t* client, scenario_result_t* result) {
    printf("=== Scenario 2: Matching Trade ===\n\n");

    init_result(result);
    if (result) result->start_time_ns = engine_client_now_ns();

    callback_context_t ctx = { .result = result, .verbose = true };
    engine_client_set_response_callback(client, response_counter, &ctx);

    /* Buy order */
    printf("Sending: BUY IBM 50@100\n");
    uint32_t oid = engine_client_send_order(client, "IBM", 100, 50, SIDE_BUY, 0);
    if (oid > 0 && result) result->orders_sent++;
    sleep_ms(200);
    engine_client_recv_all(client, 200);

    /* Matching sell order */
    printf("\nSending: SELL IBM 50@100 (should match!)\n");
    oid = engine_client_send_order(client, "IBM", 100, 50, SIDE_SELL, 0);
    if (oid > 0 && result) result->orders_sent++;
    sleep_ms(200);
    engine_client_recv_all(client, 200);

    for (int i = 0; i < 3; i++) {
        sleep_ms(200);
        engine_client_recv_all(client, 200);
    }

    finalize_result(result, client);
    return true;
}

bool scenario_cancel_order(engine_client_t* client, scenario_result_t* result) {
    printf("=== Scenario 3: Cancel Order ===\n\n");

    init_result(result);
    if (result) result->start_time_ns = engine_client_now_ns();

    callback_context_t ctx = { .result = result, .verbose = true };
    engine_client_set_response_callback(client, response_counter, &ctx);

    /* Buy order */
    printf("Sending: BUY IBM 50@100\n");
    uint32_t oid = engine_client_send_order(client, "IBM", 100, 50, SIDE_BUY, 0);
    if (oid > 0 && result) result->orders_sent++;
    sleep_ms(200);
    engine_client_recv_all(client, 200);

    /* Cancel */
    printf("\nSending: CANCEL order %u\n", oid);
    engine_client_send_cancel(client, oid);
    sleep_ms(200);
    engine_client_recv_all(client, 200);

    for (int i = 0; i < 3; i++) {
        sleep_ms(200);
        engine_client_recv_all(client, 200);
    }

    finalize_result(result, client);
    return true;
}

/* ============================================================
 * Small Stress Test (Non-Matching, up to 100K)
 * ============================================================ */

bool scenario_stress_test(engine_client_t* client, uint32_t count,
                          scenario_result_t* result) {
    printf("=== Stress Test: %u Orders (non-matching) ===\n\n", count);

    init_result(result);
    if (result) result->start_time_ns = engine_client_now_ns();

    callback_context_t ctx = { .result = result, .verbose = false };
    engine_client_set_response_callback(client, response_counter, &ctx);

    /* Flush first */
    engine_client_send_flush(client);
    sleep_ms(100);
    engine_client_recv_all(client, 50);

    /* Progress tracking */
    uint32_t progress_interval = count / 20;
    if (progress_interval == 0) progress_interval = 1;
    uint32_t last_progress = 0;

    uint64_t start_time = engine_client_now_ns();

    /* Send orders */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t price = 100 + (i % 100);

        uint32_t oid = engine_client_send_order(client, "IBM", price, 10, SIDE_BUY, 0);

        if (oid > 0) {
            if (result) result->orders_sent++;
        } else {
            if (result) result->orders_failed++;
        }

        /* Progress indicator */
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
    sleep_ms(2000);
    engine_client_recv_all(client, 100);

    finalize_result(result, client);
    scenario_print_result(result);

    return true;
}

/* ============================================================
 * Matching Stress (Single Symbol)
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
    sleep_ms(200);
    engine_client_recv_all(client, 200);

    /* Determine batch size and delay for large tests */
    uint32_t batch_size;
    int delay_ms;

    if (pairs >= 10000000) {
        batch_size = 100000;
        delay_ms = 50;
        printf("Batched mode: %u pairs/batch, %d ms delay\n\n", batch_size, delay_ms);
    } else if (pairs >= 1000000) {
        batch_size = 50000;
        delay_ms = 25;
        printf("Batched mode: %u pairs/batch, %d ms delay\n\n", batch_size, delay_ms);
    } else {
        batch_size = pairs;
        delay_ms = 0;
    }

    /* Progress tracking - 5% increments */
    uint32_t progress_interval = pairs / 20;
    if (progress_interval == 0) progress_interval = 1;
    uint32_t last_progress = 0;

    uint64_t start_time = engine_client_now_ns();

    /* Send matching pairs */
    for (uint32_t i = 0; i < pairs; i++) {
        uint32_t price = 100 + (i % 50);

        /* Buy order */
        uint32_t buy_oid = engine_client_send_order(client, "IBM", price, 10, SIDE_BUY, 0);
        if (buy_oid > 0 && result) result->orders_sent++;

        /* Matching sell order */
        uint32_t sell_oid = engine_client_send_order(client, "IBM", price, 10, SIDE_SELL, 0);
        if (sell_oid > 0 && result) result->orders_sent++;

        /* Progress indicator */
        if (i > 0 && i / progress_interval > last_progress) {
            last_progress = i / progress_interval;
            uint32_t pct = (i * 100) / pairs;
            uint64_t elapsed_ns = engine_client_now_ns() - start_time;
            uint64_t elapsed_ms = elapsed_ns / 1000000;
            uint64_t orders_sent = (uint64_t)i * 2;
            uint64_t rate = (elapsed_ms > 0) ? (orders_sent * 1000 / elapsed_ms) : 0;
            printf("  %u%% (%u pairs, %lu ms, %lu orders/sec)\n",
                   pct, i, (unsigned long)elapsed_ms, (unsigned long)rate);
        }

        /* Batch delay for large tests */
        if (delay_ms > 0 && i > 0 && (i % batch_size) == 0) {
            sleep_ms(delay_ms);
        }
    }

    /* Wait for responses - scale with size */
    int wait_ms = 500;
    if (pairs >= 10000000) {
        wait_ms = 5000;
    } else if (pairs >= 1000000) {
        wait_ms = 2000;
    }
    printf("\nWaiting %d ms for server to finish processing...\n", wait_ms);
    sleep_ms(wait_ms);
    engine_client_recv_all(client, 200);

    finalize_result(result, client);
    scenario_print_result(result);

    return true;
}

/* ============================================================
 * Multi-Symbol Matching Stress (Dual Processor)
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

    /* Flush first */
    printf("Flushing existing orders...\n");
    engine_client_send_flush(client);
    sleep_ms(500);
    engine_client_recv_all(client, 200);

    /* Batch configuration for extreme scale */
    uint32_t batch_size;
    int delay_ms;

    if (pairs >= 100000000) {
        batch_size = 500000;
        delay_ms = 100;
    } else if (pairs >= 10000000) {
        batch_size = 100000;
        delay_ms = 50;
    } else if (pairs >= 1000000) {
        batch_size = 50000;
        delay_ms = 25;
    } else {
        batch_size = pairs;
        delay_ms = 0;
    }

    printf("Batched mode: %u pairs/batch, %d ms delay\n", batch_size, delay_ms);
    printf("Starting benchmark...\n\n");

    /* Progress tracking - 5% increments */
    uint32_t progress_interval = pairs / 20;
    if (progress_interval == 0) progress_interval = 1;
    uint32_t last_progress = 0;

    /* Track processor distribution */
    uint32_t proc0_count = 0;
    uint32_t proc1_count = 0;

    uint64_t start_time = engine_client_now_ns();

    /* Send matching pairs across all symbols */
    for (uint32_t i = 0; i < pairs; i++) {
        /* Rotate through symbols to balance load */
        int symbol_idx = i % NUM_DUAL_PROC_SYMBOLS;
        const char* symbol = DUAL_PROC_SYMBOLS[symbol_idx];
        uint32_t price = 100 + (i % 50);

        /* Buy order */
        uint32_t buy_oid = engine_client_send_order(client, symbol, price, 10, SIDE_BUY, 0);
        if (buy_oid > 0) {
            if (result) result->orders_sent++;
            if (symbol_idx < 5) proc0_count++;
            else proc1_count++;
        }

        /* Matching sell order */
        uint32_t sell_oid = engine_client_send_order(client, symbol, price, 10, SIDE_SELL, 0);
        if (sell_oid > 0) {
            if (result) result->orders_sent++;
            if (symbol_idx < 5) proc0_count++;
            else proc1_count++;
        }

        /* Progress indicator */
        if (i > 0 && i / progress_interval > last_progress) {
            last_progress = i / progress_interval;
            uint32_t pct = (i * 100) / pairs;
            uint64_t elapsed_ns = engine_client_now_ns() - start_time;
            uint64_t elapsed_ms = elapsed_ns / 1000000;
            uint64_t elapsed_sec = elapsed_ms / 1000;
            uint64_t orders_sent = (uint64_t)i * 2;
            uint64_t rate = (elapsed_ms > 0) ? (orders_sent * 1000 / elapsed_ms) : 0;
            
            if (elapsed_sec >= 60) {
                uint64_t mins = elapsed_sec / 60;
                uint64_t secs = elapsed_sec % 60;
                printf("  %3u%% | %10u pairs | %2lu:%02lu elapsed | %lu orders/sec\n",
                       pct, i, (unsigned long)mins, (unsigned long)secs, (unsigned long)rate);
            } else {
                printf("  %3u%% | %10u pairs | %5lu ms | %lu orders/sec\n",
                       pct, i, (unsigned long)elapsed_ms, (unsigned long)rate);
            }
        }

        /* Batch delay */
        if (delay_ms > 0 && i > 0 && (i % batch_size) == 0) {
            sleep_ms(delay_ms);
        }
    }

    /* Store processor distribution */
    if (result) {
        result->proc0_orders = proc0_count;
        result->proc1_orders = proc1_count;
    }

    /* Final timing */
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
    printf("============================================================\n\n");

    /* Wait for server to finish processing */
    int wait_ms = 5000;
    if (pairs >= 100000000) {
        wait_ms = 30000;
    } else if (pairs >= 10000000) {
        wait_ms = 10000;
    }
    printf("Waiting %d ms for server to finish processing...\n", wait_ms);
    sleep_ms(wait_ms);
    engine_client_recv_all(client, 200);

    finalize_result(result, client);
    scenario_print_result(result);

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

    /* Reset client stats */
    engine_client_reset_stats(client);
    engine_client_reset_order_id(client, 1);

    switch (scenario_id) {
        /* Basic */
        case 1:  return scenario_simple_orders(client, result);
        case 2:  return scenario_matching_trade(client, result);
        case 3:  return scenario_cancel_order(client, result);

        /* Small stress (non-matching, up to 100K) */
        case 10: return scenario_stress_test(client, 1000, result);
        case 11: return scenario_stress_test(client, 10000, result);
        case 12: return scenario_stress_test(client, 100000, result);

        /* Matching - single symbol */
        case 20: return scenario_matching_stress(client, 1000, result);
        case 21: return scenario_matching_stress(client, 10000, result);
        case 22: return scenario_matching_stress(client, 100000, result);
        case 23: return scenario_matching_stress(client, 1000000, result);
        case 24: return scenario_matching_stress(client, 10000000, result);
        case 25: return scenario_matching_stress(client, 50000000, result);

        /* Matching - multi-symbol (dual processor) */
        case 26: return scenario_multi_symbol_matching_stress(client, 250000000, result);
        case 27: return scenario_multi_symbol_matching_stress(client, 500000000, result);

        default:
            fprintf(stderr, "Scenario %d not implemented\n", scenario_id);
            return false;
    }
}

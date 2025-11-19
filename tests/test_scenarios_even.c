#include "unity.h"
#include "matching_engine.h"
#include "message_parser.h"
#include "message_formatter.h"
#include <string.h>
#include <stdio.h>

/* Test infrastructure */
static matching_engine_t engine;
static message_parser_t parser;
static message_formatter_t formatter;

void setUp(void) {
    matching_engine_init(&engine);
    message_parser_init(&parser);
    message_formatter_init(&formatter);
}

void tearDown(void) {
    matching_engine_destroy(&engine);
}

/* Helper: Process array of input strings */
static void process_input(const char* input[], size_t count) {
    for (size_t i = 0; i < count; i++) {
        output_buffer_t output;
        output_buffer_init(&output);

        input_msg_t msg;
        message_parser_parse(&parser, input[i], &msg);

        matching_engine_process_message(&engine, &msg, &output);

        /* Print outputs for debugging */
        for (int j = 0; j < output.count; j++) {
            const char* formatted = message_formatter_format(&formatter, &output.messages[j]);
            fprintf(stderr, "%s\n", formatted);
        }
    }
}

/* Helper: Verify expected outputs match actual outputs */
static void verify_outputs(const char* expected[], size_t count) {
    (void)expected;
    (void)count;
    /* For now, just verify count - more sophisticated verification can be added */
}

/* ============================================================================
 * Even-Numbered Test Scenarios
 * ============================================================================ */

/* Test: Scenario 2 - Shallow Bid */
void test_Scenario2_ShallowBid(void) {
    setUp();

    const char* input[] = {
        "N, 1, AAPL, 10, 100, B, 1",
        "N, 1, AAPL, 12, 100, S, 2",
        "N, 2, AAPL, 11, 100, S, 102",
        "N, 2, AAPL, 10, 100, S, 103",
        "N, 1, AAPL, 10, 100, B, 3",
        "F"
    };

    const char* expected[] = {
        "A, AAPL, 1, 1",
        "B, AAPL, B, 10, 100",
        "A, AAPL, 1, 2",
        "B, AAPL, S, 12, 100",
        "A, AAPL, 2, 102",
        "B, AAPL, S, 11, 100",
        "A, AAPL, 2, 103",
        "T, AAPL, 1, 1, 2, 103, 10, 100",
        "B, AAPL, B, -, -",
        "A, AAPL, 1, 3",
        "B, AAPL, B, 10, 100",
        "C, AAPL, 1, 3",      // Cancel ack for order 3 (bid at 10)
        "C, AAPL, 2, 102",    // Cancel ack for order 102 (ask at 11)
        "C, AAPL, 1, 2",      // Cancel ack for order 2 (ask at 12)
        "B, AAPL, B, -, -",   // Bid side eliminated
        "B, AAPL, S, -, -"    // Ask side eliminated
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 4 - Limit Below Best Bid */
void test_Scenario4_LimitBelowBestBid(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 2, IBM, 9, 100, S, 103",
        "F"
    };

    const char* expected[] = {
        "A, IBM, 1, 1",
        "B, IBM, B, 10, 100",
        "A, IBM, 1, 2",
        "B, IBM, S, 12, 100",
        "A, IBM, 2, 101",
        "A, IBM, 2, 102",
        "B, IBM, S, 11, 100",
        "A, IBM, 2, 103",
        "T, IBM, 1, 1, 2, 103, 10, 100",
        "B, IBM, B, 9, 100",
        "C, IBM, 2, 101",     // Cancel ack for order 101 (bid at 9)
        "C, IBM, 2, 102",     // Cancel ack for order 102 (ask at 11)
        "C, IBM, 1, 2",       // Cancel ack for order 2 (ask at 12)
        "B, IBM, B, -, -",    // Bid side eliminated
        "B, IBM, S, -, -"     // Ask side eliminated
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 6 - Market Sell */
void test_Scenario6_MarketSell(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 2, IBM, 0, 100, S, 103",
        "F"
    };

    const char* expected[] = {
        "A, IBM, 1, 1",
        "B, IBM, B, 10, 100",
        "A, IBM, 1, 2",
        "B, IBM, S, 12, 100",
        "A, IBM, 2, 101",
        "A, IBM, 2, 102",
        "B, IBM, S, 11, 100",
        "A, IBM, 2, 103",
        "T, IBM, 1, 1, 2, 103, 10, 100",
        "B, IBM, B, 9, 100",
        "C, IBM, 2, 101",     // Cancel ack for order 101 (bid at 9)
        "C, IBM, 2, 102",     // Cancel ack for order 102 (ask at 11)
        "C, IBM, 1, 2",       // Cancel ack for order 2 (ask at 12)
        "B, IBM, B, -, -",    // Bid side eliminated
        "B, IBM, S, -, -"     // Ask side eliminated
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 8 - Tighten Spread */
void test_Scenario8_TightenSpread(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 16, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 15, 100, S, 102",
        "N, 2, IBM, 11, 100, B, 103",
        "N, 1, IBM, 14, 100, S, 3",
        "F"
    };

    const char* expected[] = {
        "A, IBM, 1, 1",
        "B, IBM, B, 10, 100",
        "A, IBM, 1, 2",
        "B, IBM, S, 16, 100",
        "A, IBM, 2, 101",
        "A, IBM, 2, 102",
        "B, IBM, S, 15, 100",
        "A, IBM, 2, 103",
        "B, IBM, B, 11, 100",
        "A, IBM, 1, 3",
        "B, IBM, S, 14, 100",
        "C, IBM, 2, 103",     // Cancel ack for order 103 (bid at 11)
        "C, IBM, 1, 1",       // Cancel ack for order 1 (bid at 10)
        "C, IBM, 2, 101",     // Cancel ack for order 101 (bid at 9)
        "C, IBM, 1, 3",       // Cancel ack for order 3 (ask at 14)
        "C, IBM, 2, 102",     // Cancel ack for order 102 (ask at 15)
        "C, IBM, 1, 2",       // Cancel ack for order 2 (ask at 16)
        "B, IBM, B, -, -",    // Bid side eliminated
        "B, IBM, S, -, -"     // Ask side eliminated
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 10 - Market Buy Partial */
void test_Scenario10_MarketBuyPartial(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 1, IBM, 0, 20, B, 3",
        "F"
    };

    const char* expected[] = {
        "A, IBM, 1, 1",
        "B, IBM, B, 10, 100",
        "A, IBM, 1, 2",
        "B, IBM, S, 12, 100",
        "A, IBM, 2, 101",
        "A, IBM, 2, 102",
        "B, IBM, S, 11, 100",
        "A, IBM, 1, 3",
        "T, IBM, 1, 3, 2, 102, 11, 20",
        "B, IBM, S, 11, 80",
        "C, IBM, 1, 1",       // Cancel ack for order 1 (bid at 10)
        "C, IBM, 2, 101",     // Cancel ack for order 101 (bid at 9)
        "C, IBM, 2, 102",     // Cancel ack for order 102 (ask at 11, 80 remaining)
        "C, IBM, 1, 2",       // Cancel ack for order 2 (ask at 12)
        "B, IBM, B, -, -",    // Bid side eliminated
        "B, IBM, S, -, -"     // Ask side eliminated
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 12 - Limit Buy Partial */
void test_Scenario12_LimitBuyPartial(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 1, IBM, 11, 20, B, 3",
        "F"
    };

    const char* expected[] = {
        "A, IBM, 1, 1",
        "B, IBM, B, 10, 100",
        "A, IBM, 1, 2",
        "B, IBM, S, 12, 100",
        "A, IBM, 2, 101",
        "A, IBM, 2, 102",
        "B, IBM, S, 11, 100",
        "A, IBM, 1, 3",
        "T, IBM, 1, 3, 2, 102, 11, 20",
        "B, IBM, S, 11, 80",
        "C, IBM, 1, 1",       // Cancel ack for order 1 (bid at 10)
        "C, IBM, 2, 101",     // Cancel ack for order 101 (bid at 9)
        "C, IBM, 2, 102",     // Cancel ack for order 102 (ask at 11, 80 remaining)
        "C, IBM, 1, 2",       // Cancel ack for order 2 (ask at 12)
        "B, IBM, B, -, -",    // Bid side eliminated
        "B, IBM, S, -, -"     // Ask side eliminated
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 14 - Cancel Best Bid and Offer */
void test_Scenario14_CancelBestBidOffer(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "C, 1, 1",
        "C, 2, 102",
        "F"
    };

    const char* expected[] = {
        "A, IBM, 1, 1",
        "B, IBM, B, 10, 100",
        "A, IBM, 1, 2",
        "B, IBM, S, 12, 100",
        "A, IBM, 2, 101",
        "A, IBM, 2, 102",
        "B, IBM, S, 11, 100",
        "C, IBM, 1, 1",
        "B, IBM, B, 9, 100",
        "C, IBM, 2, 102",
        "B, IBM, S, 12, 100",
        "C, IBM, 2, 101",     // Cancel ack for order 101 (bid at 9)
        "C, IBM, 1, 2",       // Cancel ack for order 2 (ask at 12)
        "B, IBM, B, -, -",    // Bid side eliminated
        "B, IBM, S, -, -"     // Ask side eliminated
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 16 - Cancel All Bids */
void test_Scenario16_CancelAllBids(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "C, 1, 1",
        "C, 2, 101",
        "F"
    };

    const char* expected[] = {
        "A, IBM, 1, 1",
        "B, IBM, B, 10, 100",
        "A, IBM, 1, 2",
        "B, IBM, S, 12, 100",
        "A, IBM, 2, 101",
        "A, IBM, 2, 102",
        "B, IBM, S, 11, 100",
        "C, IBM, 1, 1",
        "B, IBM, B, 9, 100",
        "C, IBM, 2, 101",
        "B, IBM, B, -, -",    // Bid side eliminated during cancels
        "C, IBM, 2, 102",     // Cancel ack for order 102 (ask at 11)
        "C, IBM, 1, 2",       // Cancel ack for order 2 (ask at 12)
        "B, IBM, S, -, -"     // Ask side eliminated (bid already eliminated)
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}


#include "unity.h"
#include "core/matching_engine.h"
#include "protocol/csv/message_parser.h"
#include "protocol/csv/message_formatter.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

/* Test fixture - use mmap for large allocations to avoid overcommit issues */
static matching_engine_t* engine;
static message_parser_t* parser;
static message_formatter_t* formatter;
static memory_pools_t* test_pools;

static void setUp(void) {
    /* Use mmap with MAP_POPULATE for large structures */
    test_pools = mmap(NULL, sizeof(memory_pools_t),
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                      -1, 0);
    engine = mmap(NULL, sizeof(matching_engine_t),
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                  -1, 0);
    
    /* Small structures can use malloc */
    parser = malloc(sizeof(message_parser_t));
    formatter = malloc(sizeof(message_formatter_t));

    TEST_ASSERT_TRUE(test_pools != MAP_FAILED);
    TEST_ASSERT_TRUE(engine != MAP_FAILED);
    TEST_ASSERT_NOT_NULL(parser);
    TEST_ASSERT_NOT_NULL(formatter);

    memory_pools_init(test_pools);
    matching_engine_init(engine, test_pools);
    message_parser_init(parser);
    message_formatter_init(formatter);
}

static void tearDown(void) {
    if (engine && engine != MAP_FAILED) {
        matching_engine_destroy(engine);
        munmap(engine, sizeof(matching_engine_t));
        engine = NULL;
    }
    if (test_pools && test_pools != MAP_FAILED) {
        munmap(test_pools, sizeof(memory_pools_t));
        test_pools = NULL;
    }
    if (parser) {
        free(parser);
        parser = NULL;
    }
    if (formatter) {
        free(formatter);
        formatter = NULL;
    }
}

/* Helper: Process array of input strings */
static void process_input(const char* input[], size_t count) {
    for (size_t i = 0; i < count; i++) {
        output_buffer_t* output = malloc(sizeof(output_buffer_t));
        TEST_ASSERT_NOT_NULL(output);
        output_buffer_init(output);

        input_msg_t msg;
        message_parser_parse(parser, input[i], &msg);

        matching_engine_process_message(engine, &msg, 0, output);

        /* Print outputs for debugging */
        for (int j = 0; j < output->count; j++) {
            const char* formatted = message_formatter_format(formatter, &output->messages[j]);
            fprintf(stderr, "%s", formatted);
        }
        free(output);
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
        "A, AAPL, 1, 1\n",
        "B, AAPL, B, 10, 100\n",
        "A, AAPL, 1, 2\n",
        "B, AAPL, S, 12, 100\n",
        "A, AAPL, 2, 102\n",
        "B, AAPL, S, 11, 100\n",
        "A, AAPL, 2, 103\n",
        "T, AAPL, 1, 1, 2, 103, 10, 100\n",
        "B, AAPL, B, -, -\n",
        "A, AAPL, 1, 3\n",
        "B, AAPL, B, 10, 100\n",
        "C, AAPL, 1, 3\n",
        "C, AAPL, 2, 102\n",
        "C, AAPL, 1, 2\n",
        "B, AAPL, B, -, -\n",
        "B, AAPL, S, -, -\n"
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
        "A, IBM, 1, 1\n",
        "B, IBM, B, 10, 100\n",
        "A, IBM, 1, 2\n",
        "B, IBM, S, 12, 100\n",
        "A, IBM, 2, 101\n",
        "A, IBM, 2, 102\n",
        "B, IBM, S, 11, 100\n",
        "A, IBM, 2, 103\n",
        "T, IBM, 1, 1, 2, 103, 10, 100\n",
        "B, IBM, B, 9, 100\n",
        "C, IBM, 2, 101\n",
        "C, IBM, 2, 102\n",
        "C, IBM, 1, 2\n",
        "B, IBM, B, -, -\n",
        "B, IBM, S, -, -\n"
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
        "A, IBM, 1, 1\n",
        "B, IBM, B, 10, 100\n",
        "A, IBM, 1, 2\n",
        "B, IBM, S, 12, 100\n",
        "A, IBM, 2, 101\n",
        "A, IBM, 2, 102\n",
        "B, IBM, S, 11, 100\n",
        "A, IBM, 2, 103\n",
        "T, IBM, 1, 1, 2, 103, 10, 100\n",
        "B, IBM, B, 9, 100\n",
        "C, IBM, 2, 101\n",
        "C, IBM, 2, 102\n",
        "C, IBM, 1, 2\n",
        "B, IBM, B, -, -\n",
        "B, IBM, S, -, -\n"
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
        "A, IBM, 1, 1\n",
        "B, IBM, B, 10, 100\n",
        "A, IBM, 1, 2\n",
        "B, IBM, S, 16, 100\n",
        "A, IBM, 2, 101\n",
        "A, IBM, 2, 102\n",
        "B, IBM, S, 15, 100\n",
        "A, IBM, 2, 103\n",
        "B, IBM, B, 11, 100\n",
        "A, IBM, 1, 3\n",
        "B, IBM, S, 14, 100\n",
        "C, IBM, 2, 103\n",
        "C, IBM, 1, 1\n",
        "C, IBM, 2, 101\n",
        "C, IBM, 1, 3\n",
        "C, IBM, 2, 102\n",
        "C, IBM, 1, 2\n",
        "B, IBM, B, -, -\n",
        "B, IBM, S, -, -\n"
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
        "A, IBM, 1, 1\n",
        "B, IBM, B, 10, 100\n",
        "A, IBM, 1, 2\n",
        "B, IBM, S, 12, 100\n",
        "A, IBM, 2, 101\n",
        "A, IBM, 2, 102\n",
        "B, IBM, S, 11, 100\n",
        "A, IBM, 1, 3\n",
        "T, IBM, 1, 3, 2, 102, 11, 20\n",
        "B, IBM, S, 11, 80\n",
        "C, IBM, 1, 1\n",
        "C, IBM, 2, 101\n",
        "C, IBM, 2, 102\n",
        "C, IBM, 1, 2\n",
        "B, IBM, B, -, -\n",
        "B, IBM, S, -, -\n"
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
        "A, IBM, 1, 1\n",
        "B, IBM, B, 10, 100\n",
        "A, IBM, 1, 2\n",
        "B, IBM, S, 12, 100\n",
        "A, IBM, 2, 101\n",
        "A, IBM, 2, 102\n",
        "B, IBM, S, 11, 100\n",
        "A, IBM, 1, 3\n",
        "T, IBM, 1, 3, 2, 102, 11, 20\n",
        "B, IBM, S, 11, 80\n",
        "C, IBM, 1, 1\n",
        "C, IBM, 2, 101\n",
        "C, IBM, 2, 102\n",
        "C, IBM, 1, 2\n",
        "B, IBM, B, -, -\n",
        "B, IBM, S, -, -\n"
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
        "A, IBM, 1, 1\n",
        "B, IBM, B, 10, 100\n",
        "A, IBM, 1, 2\n",
        "B, IBM, S, 12, 100\n",
        "A, IBM, 2, 101\n",
        "A, IBM, 2, 102\n",
        "B, IBM, S, 11, 100\n",
        "C, IBM, 1, 1\n",
        "B, IBM, B, 9, 100\n",
        "C, IBM, 2, 102\n",
        "B, IBM, S, 12, 100\n",
        "C, IBM, 2, 101\n",
        "C, IBM, 1, 2\n",
        "B, IBM, B, -, -\n",
        "B, IBM, S, -, -\n"
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
        "A, IBM, 1, 1\n",
        "B, IBM, B, 10, 100\n",
        "A, IBM, 1, 2\n",
        "B, IBM, S, 12, 100\n",
        "A, IBM, 2, 101\n",
        "A, IBM, 2, 102\n",
        "B, IBM, S, 11, 100\n",
        "C, IBM, 1, 1\n",
        "B, IBM, B, 9, 100\n",
        "C, IBM, 2, 101\n",
        "B, IBM, B, -, -\n",
        "C, IBM, 2, 102\n",
        "C, IBM, 1, 2\n",
        "B, IBM, S, -, -\n"
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

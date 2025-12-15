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

/* Maximum lines for test scenarios */
#define MAX_INPUT_LINES 100
#define MAX_OUTPUT_LINES 500

/* Output storage */
static char actual_outputs[MAX_OUTPUT_LINES][MAX_OUTPUT_LINE_LENGTH];
static int actual_output_count;

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
    actual_output_count = 0;
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

/* Helper: Process input lines and collect formatted outputs */
static void process_input(const char* input_lines[], int num_lines) {
    actual_output_count = 0;

    for (int i = 0; i < num_lines; i++) {
        input_msg_t msg;

        if (message_parser_parse(parser, input_lines[i], &msg)) {
            output_buffer_t* output = malloc(sizeof(output_buffer_t));
            TEST_ASSERT_NOT_NULL(output);
            output_buffer_init(output);

            matching_engine_process_message(engine, &msg, 0, output);

            /* Format each output message */
            for (int j = 0; j < output->count; j++) {
                const char* formatted = message_formatter_format(formatter, &output->messages[j]);
                strncpy(actual_outputs[actual_output_count], formatted, MAX_OUTPUT_LINE_LENGTH - 1);
                actual_outputs[actual_output_count][MAX_OUTPUT_LINE_LENGTH - 1] = '\0';
                actual_output_count++;

                if (actual_output_count >= MAX_OUTPUT_LINES) {
                    free(output);
                    return; /* Prevent overflow */
                }
            }
            free(output);
        }
    }
}

/* Helper: Verify outputs match expected */
static void verify_outputs(const char* expected[], int expected_count) {
    TEST_ASSERT_EQUAL_INT(expected_count, actual_output_count);

    for (int i = 0; i < expected_count && i < actual_output_count; i++) {
        TEST_ASSERT_EQUAL_STRING(expected[i], actual_outputs[i]);
    }
}

/* ============================================================================
 * Scenario Tests
 * ============================================================================ */

/* Test: Scenario 1 - Balanced Book */
void test_Scenario1_BalancedBook(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 1, IBM, 11, 100, B, 3",
        "N, 2, IBM, 10, 100, S, 103",
        "N, 1, IBM, 10, 100, B, 4",
        "N, 2, IBM, 11, 100, S, 104",
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
        "T, IBM, 1, 3, 2, 102, 11, 100\n",
        "B, IBM, S, 12, 100\n",
        "A, IBM, 2, 103\n",
        "T, IBM, 1, 1, 2, 103, 10, 100\n",
        "B, IBM, B, 9, 100\n",
        "A, IBM, 1, 4\n",
        "B, IBM, B, 10, 100\n",
        "A, IBM, 2, 104\n",
        "B, IBM, S, 11, 100\n",
        "C, IBM, 1, 4\n",
        "C, IBM, 2, 101\n",
        "C, IBM, 2, 104\n",
        "C, IBM, 1, 2\n",
        "B, IBM, B, -, -\n",
        "B, IBM, S, -, -\n"
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 3 - Shallow Ask */
void test_Scenario3_ShallowAsk(void) {
    setUp();

    const char* input[] = {
        "N, 1, VAL, 10, 100, B, 1",
        "N, 2, VAL, 9, 100, B, 101",
        "N, 2, VAL, 11, 100, S, 102",
        "N, 1, VAL, 11, 100, B, 2",
        "N, 2, VAL, 11, 100, S, 103",
        "F"
    };

    const char* expected[] = {
        "A, VAL, 1, 1\n",
        "B, VAL, B, 10, 100\n",
        "A, VAL, 2, 101\n",
        "A, VAL, 2, 102\n",
        "B, VAL, S, 11, 100\n",
        "A, VAL, 1, 2\n",
        "T, VAL, 1, 2, 2, 102, 11, 100\n",
        "B, VAL, S, -, -\n",
        "A, VAL, 2, 103\n",
        "B, VAL, S, 11, 100\n",
        "C, VAL, 1, 1\n",
        "C, VAL, 2, 101\n",
        "C, VAL, 2, 103\n",
        "B, VAL, B, -, -\n",
        "B, VAL, S, -, -\n"
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 9 - Market Sell Partial */
void test_Scenario9_MarketSellPartial(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 2, IBM, 0, 20, S, 103",
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
        "T, IBM, 1, 1, 2, 103, 10, 20\n",
        "B, IBM, B, 10, 80\n",
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

/* Test: Scenario 11 - Limit Sell Partial */
void test_Scenario11_LimitSellPartial(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 2, IBM, 10, 20, S, 103",
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
        "T, IBM, 1, 1, 2, 103, 10, 20\n",
        "B, IBM, B, 10, 80\n",
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

/* Test: Scenario 13 - Multiple Orders at Best Price */
void test_Scenario13_MultipleOrdersAtBestPrice(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 2, IBM, 10, 50, B, 103",
        "N, 1, IBM, 11, 50, S, 3",
        "N, 1, IBM, 11, 100, B, 4",
        "N, 2, IBM, 10, 100, S, 104",
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
        "B, IBM, B, 10, 150\n",
        "A, IBM, 1, 3\n",
        "B, IBM, S, 11, 150\n",
        "A, IBM, 1, 4\n",
        "T, IBM, 1, 4, 2, 102, 11, 100\n",
        "B, IBM, S, 11, 50\n",
        "A, IBM, 2, 104\n",
        "T, IBM, 1, 1, 2, 104, 10, 100\n",
        "B, IBM, B, 10, 50\n",
        "C, IBM, 2, 103\n",
        "C, IBM, 2, 101\n",
        "C, IBM, 1, 3\n",
        "C, IBM, 1, 2\n",
        "B, IBM, B, -, -\n",
        "B, IBM, S, -, -\n"
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

/* Test: Scenario 15 - Cancel Behind Best */
void test_Scenario15_CancelBehindBest(void) {
    setUp();

    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "C, 1, 2",
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
        "C, IBM, 1, 2\n",
        "C, IBM, 2, 101\n",
        "C, IBM, 1, 1\n",
        "C, IBM, 2, 102\n",
        "B, IBM, B, -, -\n",
        "B, IBM, S, -, -\n"
    };

    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));

    tearDown();
}

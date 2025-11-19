#include "unity.h"
#include "matching_engine.h"
#include "message_parser.h"
#include "message_formatter.h"
#include <string.h>
#include <stdio.h>

/* Test fixture */
static matching_engine_t engine;
static message_parser_t parser;
static message_formatter_t formatter;

/* Maximum lines for test scenarios */
#define MAX_INPUT_LINES 100
#define MAX_OUTPUT_LINES 500

/* Output storage */
static char actual_outputs[MAX_OUTPUT_LINES][MAX_OUTPUT_LINE_LENGTH];
static int actual_output_count;

void setUp(void) {
    matching_engine_init(&engine);
    message_parser_init(&parser);
    message_formatter_init(&formatter);
    actual_output_count = 0;
}

void tearDown(void) {
    matching_engine_destroy(&engine);
}

/* Helper: Process input lines and collect formatted outputs */
static void process_input(const char* input_lines[], int num_lines) {
    actual_output_count = 0;
    
    for (int i = 0; i < num_lines; i++) {
        input_msg_t msg;
        
        if (message_parser_parse(&parser, input_lines[i], &msg)) {
            output_buffer_t output;
            output_buffer_init(&output);
            
            matching_engine_process_message(&engine, &msg, &output);
            
            /* Format each output message */
            for (int j = 0; j < output.count; j++) {
                const char* formatted = message_formatter_format(&formatter, &output.messages[j]);
                strncpy(actual_outputs[actual_output_count], formatted, MAX_OUTPUT_LINE_LENGTH - 1);
                actual_outputs[actual_output_count][MAX_OUTPUT_LINE_LENGTH - 1] = '\0';
                actual_output_count++;
                
                if (actual_output_count >= MAX_OUTPUT_LINES) {
                    return; /* Prevent overflow */
                }
            }
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
        "A, 1, 1",
        "B, B, 10, 100",
        "A, 1, 2",
        "B, S, 12, 100",
        "A, 2, 101",
        "A, 2, 102",
        "B, S, 11, 100",
        "A, 1, 3",
        "T, 1, 3, 2, 102, 11, 100",
        "B, S, 12, 100",
        "A, 2, 103",
        "T, 1, 1, 2, 103, 10, 100",
        "B, B, 9, 100",
        "A, 1, 4",
        "B, B, 10, 100",
        "A, 2, 104",
        "B, S, 11, 100"
    };
    
    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));
}

/* Test: Scenario 3 - Shallow Ask */
void test_Scenario3_ShallowAsk(void) {
    const char* input[] = {
        "N, 1, VAL, 10, 100, B, 1",
        "N, 2, VAL, 9, 100, B, 101",
        "N, 2, VAL, 11, 100, S, 102",
        "N, 1, VAL, 11, 100, B, 2",
        "N, 2, VAL, 11, 100, S, 103",
        "F"
    };
    
    const char* expected[] = {
        "A, 1, 1",
        "B, B, 10, 100",
        "A, 2, 101",
        "A, 2, 102",
        "B, S, 11, 100",
        "A, 1, 2",
        "T, 1, 2, 2, 102, 11, 100",
        "B, S, -, -",
        "A, 2, 103",
        "B, S, 11, 100"
    };
    
    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));
}

/* Test: Scenario 9 - Market Sell Partial */
void test_Scenario9_MarketSellPartial(void) {
    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 2, IBM, 0, 20, S, 103",
        "F"
    };
    
    const char* expected[] = {
        "A, 1, 1",
        "B, B, 10, 100",
        "A, 1, 2",
        "B, S, 12, 100",
        "A, 2, 101",
        "A, 2, 102",
        "B, S, 11, 100",
        "A, 2, 103",
        "T, 1, 1, 2, 103, 10, 20",
        "B, B, 10, 80"
    };
    
    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));
}

/* Test: Scenario 11 - Limit Sell Partial */
void test_Scenario11_LimitSellPartial(void) {
    const char* input[] = {
        "N, 1, IBM, 10, 100, B, 1",
        "N, 1, IBM, 12, 100, S, 2",
        "N, 2, IBM, 9, 100, B, 101",
        "N, 2, IBM, 11, 100, S, 102",
        "N, 2, IBM, 10, 20, S, 103",
        "F"
    };
    
    const char* expected[] = {
        "A, 1, 1",
        "B, B, 10, 100",
        "A, 1, 2",
        "B, S, 12, 100",
        "A, 2, 101",
        "A, 2, 102",
        "B, S, 11, 100",
        "A, 2, 103",
        "T, 1, 1, 2, 103, 10, 20",
        "B, B, 10, 80"
    };
    
    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));
}

/* Test: Scenario 13 - Multiple Orders at Best Price */
void test_Scenario13_MultipleOrdersAtBestPrice(void) {
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
        "A, 1, 1",
        "B, B, 10, 100",
        "A, 1, 2",
        "B, S, 12, 100",
        "A, 2, 101",
        "A, 2, 102",
        "B, S, 11, 100",
        "A, 2, 103",
        "B, B, 10, 150",
        "A, 1, 3",
        "B, S, 11, 150",
        "A, 1, 4",
        "T, 1, 4, 2, 102, 11, 100",
        "B, S, 11, 50",
        "A, 2, 104",
        "T, 1, 1, 2, 104, 10, 100",
        "B, B, 10, 50"
    };
    
    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));
}

/* Test: Scenario 15 - Cancel Behind Best */
void test_Scenario15_CancelBehindBest(void) {
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
        "A, 1, 1",
        "B, B, 10, 100",
        "A, 1, 2",
        "B, S, 12, 100",
        "A, 2, 101",
        "A, 2, 102",
        "B, S, 11, 100",
        "C, 1, 2",
        "C, 2, 101"
    };
    
    process_input(input, sizeof(input) / sizeof(input[0]));
    verify_outputs(expected, sizeof(expected) / sizeof(expected[0]));
}

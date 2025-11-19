#include "unity.h"
#include "message_formatter.h"
#include <string.h>

/* Test fixture */
static message_formatter_t formatter;

static void setUp(void) {
    message_formatter_init(&formatter);
}

static void tearDown(void) {
    /* Nothing to clean up */
}

/* Test: Format Ack */
void test_FormatAck(void) {
    setUp();
    
    output_msg_t msg = make_ack_msg("IBM", 1, 42);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("A, IBM, 1, 42", output);
    
    tearDown();
}

/* Test: Format Cancel Ack */
void test_FormatCancelAck(void) {
    setUp();
    
    output_msg_t msg = make_cancel_ack_msg("IBM", 2, 100);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("C, IBM, 2, 100", output);
    
    tearDown();
}

/* Test: Format Trade */
void test_FormatTrade(void) {
    setUp();
    
    output_msg_t msg = make_trade_msg("IBM", 1, 10, 2, 20, 150, 100);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("T, IBM, 1, 10, 2, 20, 150, 100", output);
    
    tearDown();
}

/* Test: Format Top of Book - Bid */
void test_FormatTopOfBookBid(void) {
    setUp();
    
    output_msg_t msg = make_top_of_book_msg("IBM", SIDE_BUY, 100, 500);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("B, IBM, B, 100, 500", output);
    
    tearDown();
}

/* Test: Format Top of Book - Ask */
void test_FormatTopOfBookAsk(void) {
    setUp();
    
    output_msg_t msg = make_top_of_book_msg("IBM", SIDE_SELL, 105, 300);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("B, IBM, S, 105, 300", output);
    
    tearDown();
}

/* Test: Format Top of Book Eliminated - Bid */
void test_FormatTopOfBookEliminatedBid(void) {
    setUp();
    
    output_msg_t msg = make_top_of_book_eliminated_msg("IBM", SIDE_BUY);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("B, IBM, B, -, -", output);
    
    tearDown();
}

/* Test: Format Top of Book Eliminated - Ask */
void test_FormatTopOfBookEliminatedAsk(void) {
    setUp();
    
    output_msg_t msg = make_top_of_book_eliminated_msg("IBM", SIDE_SELL);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("B, IBM, S, -, -", output);
    
    tearDown();
}

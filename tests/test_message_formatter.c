#include "unity.h"
#include "message_formatter.h"

/* Test fixture */
static message_formatter_t formatter;

void setUp(void) {
    message_formatter_init(&formatter);
}

void tearDown(void) {
    /* Nothing to clean up */
}

/* Test: Format Ack */
void test_FormatAck(void) {
    output_msg_t msg = make_ack_msg(1, 42);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("A, 1, 42", output);
}

/* Test: Format Cancel Ack */
void test_FormatCancelAck(void) {
    output_msg_t msg = make_cancel_ack_msg(2, 100);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("C, 2, 100", output);
}

/* Test: Format Trade */
void test_FormatTrade(void) {
    output_msg_t msg = make_trade_msg(1, 10, 2, 20, 150, 100);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("T, 1, 10, 2, 20, 150, 100", output);
}

/* Test: Format Top of Book - Bid */
void test_FormatTopOfBookBid(void) {
    output_msg_t msg = make_top_of_book_msg(SIDE_BUY, 100, 500);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("B, B, 100, 500", output);
}

/* Test: Format Top of Book - Ask */
void test_FormatTopOfBookAsk(void) {
    output_msg_t msg = make_top_of_book_msg(SIDE_SELL, 105, 300);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("B, S, 105, 300", output);
}

/* Test: Format Top of Book Eliminated - Bid */
void test_FormatTopOfBookEliminatedBid(void) {
    output_msg_t msg = make_top_of_book_eliminated_msg(SIDE_BUY);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("B, B, -, -", output);
}

/* Test: Format Top of Book Eliminated - Ask */
void test_FormatTopOfBookEliminatedAsk(void) {
    output_msg_t msg = make_top_of_book_eliminated_msg(SIDE_SELL);
    const char* output = message_formatter_format(&formatter, &msg);
    
    TEST_ASSERT_EQUAL_STRING("B, S, -, -", output);
}

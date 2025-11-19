#include "unity.h"
#include "message_parser.h"
#include <string.h>

/* Test fixture */
static message_parser_t parser;

static void setUp(void) {
    message_parser_init(&parser);
}

static void tearDown(void) {
    /* Nothing to clean up */
}

/* Test: Parse New Order - Buy */
void test_ParseNewOrderBuy(void) {
    setUp();
    
    input_msg_t msg;
    bool result = message_parser_parse(&parser, "N, 1, IBM, 10, 100, B, 1", &msg);
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(INPUT_MSG_NEW_ORDER, msg.type);
    
    new_order_msg_t* order = &msg.data.new_order;
    TEST_ASSERT_EQUAL(1, order->user_id);
    TEST_ASSERT_EQUAL_STRING("IBM", order->symbol);
    TEST_ASSERT_EQUAL(10, order->price);
    TEST_ASSERT_EQUAL(100, order->quantity);
    TEST_ASSERT_EQUAL(SIDE_BUY, order->side);
    TEST_ASSERT_EQUAL(1, order->user_order_id);
    
    tearDown();
}

/* Test: Parse New Order - Sell */
void test_ParseNewOrderSell(void) {
    setUp();
    
    input_msg_t msg;
    bool result = message_parser_parse(&parser, "N, 2, AAPL, 150, 50, S, 42", &msg);
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(INPUT_MSG_NEW_ORDER, msg.type);
    
    new_order_msg_t* order = &msg.data.new_order;
    TEST_ASSERT_EQUAL(2, order->user_id);
    TEST_ASSERT_EQUAL_STRING("AAPL", order->symbol);
    TEST_ASSERT_EQUAL(150, order->price);
    TEST_ASSERT_EQUAL(50, order->quantity);
    TEST_ASSERT_EQUAL(SIDE_SELL, order->side);
    TEST_ASSERT_EQUAL(42, order->user_order_id);
    
    tearDown();
}

/* Test: Parse Market Order */
void test_ParseMarketOrder(void) {
    setUp();
    
    input_msg_t msg;
    bool result = message_parser_parse(&parser, "N, 1, IBM, 0, 100, B, 1", &msg);
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(0, msg.data.new_order.price);  /* Market order */
    
    tearDown();
}

/* Test: Parse Cancel */
void test_ParseCancel(void) {
    setUp();
    
    input_msg_t msg;
    bool result = message_parser_parse(&parser, "C, 1, 42", &msg);
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(INPUT_MSG_CANCEL, msg.type);
    
    cancel_msg_t* cancel = &msg.data.cancel;
    TEST_ASSERT_EQUAL(1, cancel->user_id);
    TEST_ASSERT_EQUAL(42, cancel->user_order_id);
    
    tearDown();
}

/* Test: Parse Flush */
void test_ParseFlush(void) {
    setUp();
    
    input_msg_t msg;
    bool result = message_parser_parse(&parser, "F", &msg);
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(INPUT_MSG_FLUSH, msg.type);
    
    tearDown();
}

/* Test: Parse Comment */
void test_ParseComment(void) {
    setUp();
    
    input_msg_t msg;
    bool result = message_parser_parse(&parser, "# This is a comment", &msg);
    
    TEST_ASSERT_FALSE(result);
    
    tearDown();
}

/* Test: Parse Blank Line */
void test_ParseBlankLine(void) {
    setUp();
    
    input_msg_t msg;
    bool result1 = message_parser_parse(&parser, "", &msg);
    bool result2 = message_parser_parse(&parser, "   ", &msg);
    
    TEST_ASSERT_FALSE(result1);
    TEST_ASSERT_FALSE(result2);
    
    tearDown();
}

/* Test: Parse With Extra Whitespace */
void test_ParseWithExtraWhitespace(void) {
    setUp();
    
    input_msg_t msg;
    bool result = message_parser_parse(&parser, "  N,  1,  IBM,  10,  100,  B,  1  ", &msg);
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("IBM", msg.data.new_order.symbol);
    
    tearDown();
}

/* Test: Parse Invalid Message */
void test_ParseInvalidMessage(void) {
    setUp();
    
    input_msg_t msg;
    bool result = message_parser_parse(&parser, "X, 1, 2, 3", &msg);
    
    TEST_ASSERT_FALSE(result);
    
    tearDown();
}

/* Test: Parse Invalid New Order */
void test_ParseInvalidNewOrder(void) {
    setUp();
    
    input_msg_t msg;
    
    /* Too few fields */
    bool result1 = message_parser_parse(&parser, "N, 1, IBM", &msg);
    TEST_ASSERT_FALSE(result1);
    
    /* Invalid side */
    bool result2 = message_parser_parse(&parser, "N, 1, IBM, 10, 100, X, 1", &msg);
    TEST_ASSERT_FALSE(result2);
    
    tearDown();
}

#include "unity.h"
#include "core/matching_engine.h"
#include <string.h>
#include <stdlib.h>

/* Test fixture - heap allocated to avoid ARM64 address range issues */
static matching_engine_t* engine;
static memory_pools_t* test_pools;

static void setUp(void) {
    test_pools = malloc(sizeof(memory_pools_t));
    engine = malloc(sizeof(matching_engine_t));
    TEST_ASSERT_NOT_NULL(test_pools);
    TEST_ASSERT_NOT_NULL(engine);
    memory_pools_init(test_pools);
    matching_engine_init(engine, test_pools);
}

static void tearDown(void) {
    matching_engine_destroy(engine);
    free(engine);
    free(test_pools);
    engine = NULL;
    test_pools = NULL;
}

/* Test: Process Single Order */
void test_ProcessSingleOrder(void) {
    setUp();
    new_order_msg_t msg = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "IBM"
    };
    input_msg_t input = make_new_order_msg(&msg);

    output_buffer_t output;
    output_buffer_init(&output);
    matching_engine_process_message(engine, &input, 0, &output);

    /* Should get ack + TOB update */
    TEST_ASSERT_GREATER_OR_EQUAL(2, output.count);
    TEST_ASSERT_EQUAL(OUTPUT_MSG_ACK, output.messages[0].type);
    tearDown();
}

/* Test: Multiple Symbols */
void test_MultipleSymbols(void) {
    /* Add orders for different symbols */
    setUp();
    new_order_msg_t ibm_buy = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "IBM"
    };
    new_order_msg_t aapl_buy = {
        .user_id = 1,
        .user_order_id = 2,
        .price = 150,
        .quantity = 30,
        .side = SIDE_BUY,
        .symbol = "AAPL"
    };

    output_buffer_t out1, out2;
    output_buffer_init(&out1);
    output_buffer_init(&out2);

    input_msg_t input1 = make_new_order_msg(&ibm_buy);
    input_msg_t input2 = make_new_order_msg(&aapl_buy);

    matching_engine_process_message(engine, &input1, 0, &out1);
    matching_engine_process_message(engine, &input2, 0, &out2);

    /* Each symbol should have its own order book */
    /* Verify by adding matching order for IBM */
    new_order_msg_t ibm_sell = {
        .user_id = 2,
        .user_order_id = 3,
        .price = 100,
        .quantity = 50,
        .side = SIDE_SELL,
        .symbol = "IBM"
    };
    input_msg_t input3 = make_new_order_msg(&ibm_sell);

    output_buffer_t out3;
    output_buffer_init(&out3);
    matching_engine_process_message(engine, &input3, 0, &out3);

    /* Should generate trade for IBM */
    bool found_trade = false;
    for (int i = 0; i < out3.count; i++) {
        if (out3.messages[i].type == OUTPUT_MSG_TRADE) {
            found_trade = true;
        }
    }
    TEST_ASSERT_TRUE(found_trade);
    tearDown();
}

/* Test: Cancel Order Across Symbols */
void test_CancelOrderAcrossSymbols(void) {
    /* Add order for IBM */
    setUp();
    new_order_msg_t ibm_buy = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "IBM"
    };
    input_msg_t input1 = make_new_order_msg(&ibm_buy);

    output_buffer_t out1;
    output_buffer_init(&out1);
    matching_engine_process_message(engine, &input1, 0, &out1);

    /* Cancel the order */
    cancel_msg_t cancel = {
        .user_id = 1,
        .user_order_id = 1,
        .symbol = "IBM"
    };
    input_msg_t input2 = make_cancel_msg(&cancel);

    output_buffer_t out2;
    output_buffer_init(&out2);
    matching_engine_process_message(engine, &input2, 0, &out2);

    /* Should get cancel ack */
    TEST_ASSERT_GREATER_OR_EQUAL(1, out2.count);
    TEST_ASSERT_EQUAL(OUTPUT_MSG_CANCEL_ACK, out2.messages[0].type);
    tearDown();
}

/* Test: Flush All Order Books */
void test_FlushAllOrderBooks(void) {
    /* Add orders for multiple symbols */
    setUp();
    new_order_msg_t ibm_buy = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "IBM"
    };
    new_order_msg_t aapl_buy = {
        .user_id = 1,
        .user_order_id = 2,
        .price = 150,
        .quantity = 30,
        .side = SIDE_BUY,
        .symbol = "AAPL"
    };

    output_buffer_t out1, out2;
    output_buffer_init(&out1);
    output_buffer_init(&out2);

    input_msg_t input1 = make_new_order_msg(&ibm_buy);
    input_msg_t input2 = make_new_order_msg(&aapl_buy);

    matching_engine_process_message(engine, &input1, 0, &out1);
    matching_engine_process_message(engine, &input2, 0, &out2);

    /* Flush all */
    input_msg_t flush = make_flush_msg();
    output_buffer_t out3;
    output_buffer_init(&out3);
    matching_engine_process_message(engine, &flush, 0, &out3);

    /* No output for flush */
    TEST_ASSERT_EQUAL(4, out3.count);

    /* After flush, adding same orders should work (no conflicts) */
    output_buffer_t out4;
    output_buffer_init(&out4);
    matching_engine_process_message(engine, &input1, 0, &out4);
    TEST_ASSERT_GREATER_OR_EQUAL(1, out4.count);
    tearDown();
}

/* Test: Isolated Order Books */
void test_IsolatedOrderBooks(void) {
    /* Orders in different symbols should not interact */
    setUp();
    new_order_msg_t ibm_buy = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "IBM"
    };
    new_order_msg_t aapl_sell = {
        .user_id = 2,
        .user_order_id = 2,
        .price = 100,
        .quantity = 50,
        .side = SIDE_SELL,
        .symbol = "AAPL"
    };

    output_buffer_t out1, out2;
    output_buffer_init(&out1);
    output_buffer_init(&out2);

    input_msg_t input1 = make_new_order_msg(&ibm_buy);
    input_msg_t input2 = make_new_order_msg(&aapl_sell);

    matching_engine_process_message(engine, &input1, 0, &out1);
    matching_engine_process_message(engine, &input2, 0, &out2);

    /* Should NOT generate a trade (different symbols) */
    bool found_trade = false;
    for (int i = 0; i < out2.count; i++) {
        if (out2.messages[i].type == OUTPUT_MSG_TRADE) {
            found_trade = true;
        }
    }
    TEST_ASSERT_FALSE(found_trade);
    tearDown();
}

/* Test: Cancel Non-Existent Order */
void test_CancelNonExistentOrderEngine(void) {
    /* Try to cancel order that was never added */
    setUp();
    cancel_msg_t cancel = {
        .user_id = 1,
        .user_order_id = 99,
        .symbol = "IBM"
    };
    input_msg_t input = make_cancel_msg(&cancel);

    output_buffer_t output;
    output_buffer_init(&output);
    matching_engine_process_message(engine, &input, 0, &output);

    /* Should still get cancel ack */
    TEST_ASSERT_EQUAL(1, output.count);
    TEST_ASSERT_EQUAL(OUTPUT_MSG_CANCEL_ACK, output.messages[0].type);
    tearDown();
}

/* Test: Same User Order ID Different Symbols */
void test_SameUserOrderIdDifferentSymbols(void) {
    /* Same user_order_id but different symbols (should be allowed) */
    setUp();
    new_order_msg_t ibm_buy = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "IBM"
    };
    new_order_msg_t aapl_buy = {
        .user_id = 1,
        .user_order_id = 1,  /* Same order ID - should be OK in different symbol */
        .price = 150,
        .quantity = 30,
        .side = SIDE_BUY,
        .symbol = "AAPL"
    };

    output_buffer_t out1, out2;
    output_buffer_init(&out1);
    output_buffer_init(&out2);

    input_msg_t input1 = make_new_order_msg(&ibm_buy);
    input_msg_t input2 = make_new_order_msg(&aapl_buy);

    matching_engine_process_message(engine, &input1, 0, &out1);
    matching_engine_process_message(engine, &input2, 0, &out2);

    /* Both should succeed */
    TEST_ASSERT_GREATER_OR_EQUAL(1, out1.count);
    TEST_ASSERT_GREATER_OR_EQUAL(1, out2.count);
    tearDown();
}

#include "unity.h"
#include "core/matching_engine.h"
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

/* Test fixture - use mmap for large allocations to avoid overcommit issues */
static matching_engine_t* engine;
static memory_pools_t* test_pools;

static void setUp(void) {
    /* Use mmap with MAP_POPULATE to actually allocate physical memory */
    test_pools = mmap(NULL, sizeof(memory_pools_t),
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                      -1, 0);
    TEST_ASSERT_TRUE(test_pools != MAP_FAILED);
    
    engine = mmap(NULL, sizeof(matching_engine_t),
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                  -1, 0);
    TEST_ASSERT_TRUE(engine != MAP_FAILED);
    
    memory_pools_init(test_pools);
    matching_engine_init(engine, test_pools);
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
}

/* Test: Process Single Order */
void test_ProcessSingleOrder(void) {
    setUp();
    
    new_order_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.user_id = 1;
    msg.user_order_id = 1;
    msg.price = 100;
    msg.quantity = 50;
    msg.side = SIDE_BUY;
    strncpy(msg.symbol, "IBM", sizeof(msg.symbol) - 1);
    
    input_msg_t input = make_new_order_msg(&msg);

    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);
    output_buffer_init(output);
    
    matching_engine_process_message(engine, &input, 0, output);

    TEST_ASSERT_GREATER_OR_EQUAL(2, output->count);
    TEST_ASSERT_EQUAL(OUTPUT_MSG_ACK, output->messages[0].type);
    
    free(output);
    tearDown();
}

/* Test: Multiple Symbols */
void test_MultipleSymbols(void) {
    setUp();
    
    new_order_msg_t ibm_buy;
    memset(&ibm_buy, 0, sizeof(ibm_buy));
    ibm_buy.user_id = 1;
    ibm_buy.user_order_id = 1;
    ibm_buy.price = 100;
    ibm_buy.quantity = 50;
    ibm_buy.side = SIDE_BUY;
    strncpy(ibm_buy.symbol, "IBM", sizeof(ibm_buy.symbol) - 1);
    
    new_order_msg_t aapl_buy;
    memset(&aapl_buy, 0, sizeof(aapl_buy));
    aapl_buy.user_id = 1;
    aapl_buy.user_order_id = 2;
    aapl_buy.price = 150;
    aapl_buy.quantity = 30;
    aapl_buy.side = SIDE_BUY;
    strncpy(aapl_buy.symbol, "AAPL", sizeof(aapl_buy.symbol) - 1);

    output_buffer_t* out1 = malloc(sizeof(output_buffer_t));
    output_buffer_t* out2 = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(out1);
    TEST_ASSERT_NOT_NULL(out2);
    output_buffer_init(out1);
    output_buffer_init(out2);

    input_msg_t input1 = make_new_order_msg(&ibm_buy);
    input_msg_t input2 = make_new_order_msg(&aapl_buy);

    matching_engine_process_message(engine, &input1, 0, out1);
    matching_engine_process_message(engine, &input2, 0, out2);

    new_order_msg_t ibm_sell;
    memset(&ibm_sell, 0, sizeof(ibm_sell));
    ibm_sell.user_id = 2;
    ibm_sell.user_order_id = 3;
    ibm_sell.price = 100;
    ibm_sell.quantity = 50;
    ibm_sell.side = SIDE_SELL;
    strncpy(ibm_sell.symbol, "IBM", sizeof(ibm_sell.symbol) - 1);
    
    input_msg_t input3 = make_new_order_msg(&ibm_sell);

    output_buffer_t* out3 = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(out3);
    output_buffer_init(out3);
    matching_engine_process_message(engine, &input3, 0, out3);

    bool found_trade = false;
    for (uint32_t i = 0; i < out3->count; i++) {
        if (out3->messages[i].type == OUTPUT_MSG_TRADE) {
            found_trade = true;
        }
    }
    TEST_ASSERT_TRUE(found_trade);
    
    free(out1);
    free(out2);
    free(out3);
    tearDown();
}

/* Test: Cancel Order Across Symbols */
void test_CancelOrderAcrossSymbols(void) {
    setUp();
    
    new_order_msg_t ibm_buy;
    memset(&ibm_buy, 0, sizeof(ibm_buy));
    ibm_buy.user_id = 1;
    ibm_buy.user_order_id = 1;
    ibm_buy.price = 100;
    ibm_buy.quantity = 50;
    ibm_buy.side = SIDE_BUY;
    strncpy(ibm_buy.symbol, "IBM", sizeof(ibm_buy.symbol) - 1);
    
    input_msg_t input1 = make_new_order_msg(&ibm_buy);

    output_buffer_t* out1 = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(out1);
    output_buffer_init(out1);
    matching_engine_process_message(engine, &input1, 0, out1);

    cancel_msg_t cancel;
    memset(&cancel, 0, sizeof(cancel));
    cancel.user_id = 1;
    cancel.user_order_id = 1;
    strncpy(cancel.symbol, "IBM", sizeof(cancel.symbol) - 1);
    
    input_msg_t input2 = make_cancel_msg(&cancel);

    output_buffer_t* out2 = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(out2);
    output_buffer_init(out2);
    matching_engine_process_message(engine, &input2, 0, out2);

    TEST_ASSERT_GREATER_OR_EQUAL(1, out2->count);
    TEST_ASSERT_EQUAL(OUTPUT_MSG_CANCEL_ACK, out2->messages[0].type);
    
    free(out1);
    free(out2);
    tearDown();
}

/* Test: Flush All Order Books */
void test_FlushAllOrderBooks(void) {
    setUp();
    
    new_order_msg_t ibm_buy;
    memset(&ibm_buy, 0, sizeof(ibm_buy));
    ibm_buy.user_id = 1;
    ibm_buy.user_order_id = 1;
    ibm_buy.price = 100;
    ibm_buy.quantity = 50;
    ibm_buy.side = SIDE_BUY;
    strncpy(ibm_buy.symbol, "IBM", sizeof(ibm_buy.symbol) - 1);
    
    new_order_msg_t aapl_buy;
    memset(&aapl_buy, 0, sizeof(aapl_buy));
    aapl_buy.user_id = 1;
    aapl_buy.user_order_id = 2;
    aapl_buy.price = 150;
    aapl_buy.quantity = 30;
    aapl_buy.side = SIDE_BUY;
    strncpy(aapl_buy.symbol, "AAPL", sizeof(aapl_buy.symbol) - 1);

    output_buffer_t* out1 = malloc(sizeof(output_buffer_t));
    output_buffer_t* out2 = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(out1);
    TEST_ASSERT_NOT_NULL(out2);
    output_buffer_init(out1);
    output_buffer_init(out2);

    input_msg_t input1 = make_new_order_msg(&ibm_buy);
    input_msg_t input2 = make_new_order_msg(&aapl_buy);

    matching_engine_process_message(engine, &input1, 0, out1);
    matching_engine_process_message(engine, &input2, 0, out2);

    input_msg_t flush = make_flush_msg();
    output_buffer_t* out3 = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(out3);
    output_buffer_init(out3);
    matching_engine_process_message(engine, &flush, 0, out3);

    TEST_ASSERT_EQUAL(4, out3->count);

    output_buffer_t* out4 = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(out4);
    output_buffer_init(out4);
    matching_engine_process_message(engine, &input1, 0, out4);
    TEST_ASSERT_GREATER_OR_EQUAL(1, out4->count);
    
    free(out1);
    free(out2);
    free(out3);
    free(out4);
    tearDown();
}

/* Test: Isolated Order Books */
void test_IsolatedOrderBooks(void) {
    setUp();
    
    new_order_msg_t ibm_buy;
    memset(&ibm_buy, 0, sizeof(ibm_buy));
    ibm_buy.user_id = 1;
    ibm_buy.user_order_id = 1;
    ibm_buy.price = 100;
    ibm_buy.quantity = 50;
    ibm_buy.side = SIDE_BUY;
    strncpy(ibm_buy.symbol, "IBM", sizeof(ibm_buy.symbol) - 1);
    
    new_order_msg_t aapl_sell;
    memset(&aapl_sell, 0, sizeof(aapl_sell));
    aapl_sell.user_id = 2;
    aapl_sell.user_order_id = 2;
    aapl_sell.price = 100;
    aapl_sell.quantity = 50;
    aapl_sell.side = SIDE_SELL;
    strncpy(aapl_sell.symbol, "AAPL", sizeof(aapl_sell.symbol) - 1);

    output_buffer_t* out1 = malloc(sizeof(output_buffer_t));
    output_buffer_t* out2 = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(out1);
    TEST_ASSERT_NOT_NULL(out2);
    output_buffer_init(out1);
    output_buffer_init(out2);

    input_msg_t input1 = make_new_order_msg(&ibm_buy);
    input_msg_t input2 = make_new_order_msg(&aapl_sell);

    matching_engine_process_message(engine, &input1, 0, out1);
    matching_engine_process_message(engine, &input2, 0, out2);

    bool found_trade = false;
    for (uint32_t i = 0; i < out2->count; i++) {
        if (out2->messages[i].type == OUTPUT_MSG_TRADE) {
            found_trade = true;
        }
    }
    TEST_ASSERT_FALSE(found_trade);
    
    free(out1);
    free(out2);
    tearDown();
}

/* Test: Cancel Non-Existent Order */
void test_CancelNonExistentOrderEngine(void) {
    setUp();
    
    cancel_msg_t cancel;
    memset(&cancel, 0, sizeof(cancel));
    cancel.user_id = 1;
    cancel.user_order_id = 99;
    strncpy(cancel.symbol, "IBM", sizeof(cancel.symbol) - 1);
    
    input_msg_t input = make_cancel_msg(&cancel);

    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);
    output_buffer_init(output);
    matching_engine_process_message(engine, &input, 0, output);

    TEST_ASSERT_EQUAL(1, output->count);
    TEST_ASSERT_EQUAL(OUTPUT_MSG_CANCEL_ACK, output->messages[0].type);
    
    free(output);
    tearDown();
}

/* Test: Same User Order ID Different Symbols */
void test_SameUserOrderIdDifferentSymbols(void) {
    setUp();
    
    new_order_msg_t ibm_buy;
    memset(&ibm_buy, 0, sizeof(ibm_buy));
    ibm_buy.user_id = 1;
    ibm_buy.user_order_id = 1;
    ibm_buy.price = 100;
    ibm_buy.quantity = 50;
    ibm_buy.side = SIDE_BUY;
    strncpy(ibm_buy.symbol, "IBM", sizeof(ibm_buy.symbol) - 1);
    
    new_order_msg_t aapl_buy;
    memset(&aapl_buy, 0, sizeof(aapl_buy));
    aapl_buy.user_id = 1;
    aapl_buy.user_order_id = 1;
    aapl_buy.price = 150;
    aapl_buy.quantity = 30;
    aapl_buy.side = SIDE_BUY;
    strncpy(aapl_buy.symbol, "AAPL", sizeof(aapl_buy.symbol) - 1);

    output_buffer_t* out1 = malloc(sizeof(output_buffer_t));
    output_buffer_t* out2 = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(out1);
    TEST_ASSERT_NOT_NULL(out2);
    output_buffer_init(out1);
    output_buffer_init(out2);

    input_msg_t input1 = make_new_order_msg(&ibm_buy);
    input_msg_t input2 = make_new_order_msg(&aapl_buy);

    matching_engine_process_message(engine, &input1, 0, out1);
    matching_engine_process_message(engine, &input2, 0, out2);

    TEST_ASSERT_GREATER_OR_EQUAL(1, out1->count);
    TEST_ASSERT_GREATER_OR_EQUAL(1, out2->count);
    
    free(out1);
    free(out2);
    tearDown();
}

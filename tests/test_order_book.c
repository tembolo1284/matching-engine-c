#include "unity.h"
#include "order_book.h"
#include <string.h>

/* Test fixture */
static order_book_t book;

static void setUp(void) {
    order_book_init(&book, "TEST");
}

static void tearDown(void) {
    order_book_destroy(&book);
}

/* Test: Add Single Buy Order */
void test_AddSingleBuyOrder(void) {
    setUp();
    
    new_order_msg_t msg = {
        .user_id = 1,
        .symbol = "TEST",
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .user_order_id = 1
    };
    
    output_buffer_t output;
    output_buffer_init(&output);
    
    order_book_add_order(&book, &msg, &output);
    
    /* Should get: 1 ack + 1 TOB update */
    TEST_ASSERT_EQUAL(2, output.count);
    
    /* First: Acknowledgement */
    TEST_ASSERT_EQUAL(OUTPUT_MSG_ACK, output.messages[0].type);
    
    /* Second: Top of book update */
    TEST_ASSERT_EQUAL(OUTPUT_MSG_TOP_OF_BOOK, output.messages[1].type);
    TEST_ASSERT_EQUAL(SIDE_BUY, output.messages[1].data.top_of_book.side);
    TEST_ASSERT_EQUAL(100, output.messages[1].data.top_of_book.price);
    TEST_ASSERT_EQUAL(50, output.messages[1].data.top_of_book.total_quantity);
    
    /* Verify book state */
    TEST_ASSERT_EQUAL(100, order_book_get_best_bid_price(&book));
    TEST_ASSERT_EQUAL(50, order_book_get_best_bid_quantity(&book));
    
    tearDown();
}

/* Test: Add Single Sell Order */
void test_AddSingleSellOrder(void) {
    setUp();
    
    new_order_msg_t msg = {1, "TEST", 105, 30, SIDE_SELL, 1};
    
    output_buffer_t output;
    output_buffer_init(&output);
    order_book_add_order(&book, &msg, &output);
    
    TEST_ASSERT_EQUAL(2, output.count);
    TEST_ASSERT_EQUAL(105, order_book_get_best_ask_price(&book));
    TEST_ASSERT_EQUAL(30, order_book_get_best_ask_quantity(&book));
    
    tearDown();
}

/* Test: Matching Buy and Sell */
void test_MatchingBuyAndSell(void) {
    setUp();
    
    /* Add sell order at 100 */
    new_order_msg_t sell = {1, "TEST", 100, 50, SIDE_SELL, 1};
    output_buffer_t output1;
    output_buffer_init(&output1);
    order_book_add_order(&book, &sell, &output1);
    
    /* Add buy order at 100 (should match) */
    new_order_msg_t buy = {2, "TEST", 100, 50, SIDE_BUY, 2};
    output_buffer_t output2;
    output_buffer_init(&output2);
    order_book_add_order(&book, &buy, &output2);
    
    /* Should get: ack + trade + TOB updates */
    TEST_ASSERT_GREATER_OR_EQUAL(2, output2.count);
    
    /* Find the trade message */
    bool found_trade = false;
    for (int i = 0; i < output2.count; i++) {
        if (output2.messages[i].type == OUTPUT_MSG_TRADE) {
            found_trade = true;
            trade_msg_t* trade = &output2.messages[i].data.trade;
            TEST_ASSERT_EQUAL(2, trade->user_id_buy);
            TEST_ASSERT_EQUAL(2, trade->user_order_id_buy);
            TEST_ASSERT_EQUAL(1, trade->user_id_sell);
            TEST_ASSERT_EQUAL(1, trade->user_order_id_sell);
            TEST_ASSERT_EQUAL(100, trade->price);
            TEST_ASSERT_EQUAL(50, trade->quantity);
        }
    }
    TEST_ASSERT_TRUE(found_trade);
    
    /* Book should be empty */
    TEST_ASSERT_EQUAL(0, order_book_get_best_bid_price(&book));
    TEST_ASSERT_EQUAL(0, order_book_get_best_ask_price(&book));
    
    tearDown();
}

/* Test: Partial Fill */
void test_PartialFill(void) {
    setUp();
    
    /* Add sell order at 100 for 100 shares */
    new_order_msg_t sell = {1, "TEST", 100, 100, SIDE_SELL, 1};
    output_buffer_t output1;
    output_buffer_init(&output1);
    order_book_add_order(&book, &sell, &output1);
    
    /* Add buy order at 100 for 30 shares (partial fill) */
    new_order_msg_t buy = {2, "TEST", 100, 30, SIDE_BUY, 2};
    output_buffer_t output2;
    output_buffer_init(&output2);
    order_book_add_order(&book, &buy, &output2);
    
    /* Should have a trade for 30 shares */
    bool found_trade = false;
    for (int i = 0; i < output2.count; i++) {
        if (output2.messages[i].type == OUTPUT_MSG_TRADE) {
            TEST_ASSERT_EQUAL(30, output2.messages[i].data.trade.quantity);
            found_trade = true;
        }
    }
    TEST_ASSERT_TRUE(found_trade);
    
    /* Sell order should have 70 remaining */
    TEST_ASSERT_EQUAL(70, order_book_get_best_ask_quantity(&book));
    
    tearDown();
}

/* Test: Market Order Buy */
void test_MarketOrderBuy(void) {
    setUp();
    
    /* Add sell order at 100 */
    new_order_msg_t sell = {1, "TEST", 100, 50, SIDE_SELL, 1};
    output_buffer_t output1;
    output_buffer_init(&output1);
    order_book_add_order(&book, &sell, &output1);
    
    /* Market buy (price = 0) */
    new_order_msg_t market_buy = {2, "TEST", 0, 50, SIDE_BUY, 2};
    output_buffer_t output2;
    output_buffer_init(&output2);
    order_book_add_order(&book, &market_buy, &output2);
    
    /* Should match at sell price (100) */
    bool found_trade = false;
    for (int i = 0; i < output2.count; i++) {
        if (output2.messages[i].type == OUTPUT_MSG_TRADE) {
            TEST_ASSERT_EQUAL(100, output2.messages[i].data.trade.price);
            TEST_ASSERT_EQUAL(50, output2.messages[i].data.trade.quantity);
            found_trade = true;
        }
    }
    TEST_ASSERT_TRUE(found_trade);
    
    tearDown();
}

/* Test: Market Order Sell */
void test_MarketOrderSell(void) {
    setUp();
    
    /* Add buy order at 100 */
    new_order_msg_t buy = {1, "TEST", 100, 50, SIDE_BUY, 1};
    output_buffer_t output1;
    output_buffer_init(&output1);
    order_book_add_order(&book, &buy, &output1);
    
    /* Market sell (price = 0) */
    new_order_msg_t market_sell = {2, "TEST", 0, 50, SIDE_SELL, 2};
    output_buffer_t output2;
    output_buffer_init(&output2);
    order_book_add_order(&book, &market_sell, &output2);
    
    /* Should match at buy price (100) */
    bool found_trade = false;
    for (int i = 0; i < output2.count; i++) {
        if (output2.messages[i].type == OUTPUT_MSG_TRADE) {
            TEST_ASSERT_EQUAL(100, output2.messages[i].data.trade.price);
            found_trade = true;
        }
    }
    TEST_ASSERT_TRUE(found_trade);
    
    tearDown();
}

/* Test: Price-Time Priority */
void test_PriceTimePriority(void) {
    setUp();
    
    /* Add three buy orders at same price */
    new_order_msg_t buy1 = {1, "TEST", 100, 10, SIDE_BUY, 1};
    new_order_msg_t buy2 = {2, "TEST", 100, 20, SIDE_BUY, 2};
    new_order_msg_t buy3 = {3, "TEST", 100, 30, SIDE_BUY, 3};
    
    output_buffer_t out1, out2, out3;
    output_buffer_init(&out1);
    output_buffer_init(&out2);
    output_buffer_init(&out3);
    
    order_book_add_order(&book, &buy1, &out1);
    order_book_add_order(&book, &buy2, &out2);
    order_book_add_order(&book, &buy3, &out3);
    
    TEST_ASSERT_EQUAL(60, order_book_get_best_bid_quantity(&book));
    
    /* Sell order should match in time priority (FIFO) */
    new_order_msg_t sell = {4, "TEST", 100, 35, SIDE_SELL, 4};
    output_buffer_t output;
    output_buffer_init(&output);
    order_book_add_order(&book, &sell, &output);
    
    /* Should get 3 trades */
    int trade_count = 0;
    for (int i = 0; i < output.count; i++) {
        if (output.messages[i].type == OUTPUT_MSG_TRADE) {
            trade_msg_t* trade = &output.messages[i].data.trade;
            if (trade_count == 0) {
                TEST_ASSERT_EQUAL(1, trade->user_order_id_buy);
                TEST_ASSERT_EQUAL(10, trade->quantity);
            } else if (trade_count == 1) {
                TEST_ASSERT_EQUAL(2, trade->user_order_id_buy);
                TEST_ASSERT_EQUAL(20, trade->quantity);
            } else if (trade_count == 2) {
                TEST_ASSERT_EQUAL(3, trade->user_order_id_buy);
                TEST_ASSERT_EQUAL(5, trade->quantity);
            }
            trade_count++;
        }
    }
    TEST_ASSERT_EQUAL(3, trade_count);
    
    /* Order 3 should have 25 remaining (30 - 5) */
    TEST_ASSERT_EQUAL(25, order_book_get_best_bid_quantity(&book));
    
    tearDown();
}

/* Test: Cancel Order */
void test_CancelOrder(void) {
    setUp();
    
    /* Add order */
    new_order_msg_t buy = {1, "TEST", 100, 50, SIDE_BUY, 1};
    output_buffer_t output1;
    output_buffer_init(&output1);
    order_book_add_order(&book, &buy, &output1);
    
    /* Cancel it */
    output_buffer_t output2;
    output_buffer_init(&output2);
    order_book_cancel_order(&book, 1, 1, &output2);
    
    /* Should get cancel ack + TOB eliminated */
    TEST_ASSERT_GREATER_OR_EQUAL(1, output2.count);
    TEST_ASSERT_EQUAL(OUTPUT_MSG_CANCEL_ACK, output2.messages[0].type);
    
    /* Book should be empty */
    TEST_ASSERT_EQUAL(0, order_book_get_best_bid_price(&book));
    
    tearDown();
}

/* Test: Cancel Non-Existent Order */
void test_CancelNonExistentOrder(void) {
    setUp();
    
    /* Try to cancel order that doesn't exist */
    output_buffer_t output;
    output_buffer_init(&output);
    order_book_cancel_order(&book, 1, 999, &output);
    
    /* Should still get cancel ack */
    TEST_ASSERT_EQUAL(1, output.count);
    TEST_ASSERT_EQUAL(OUTPUT_MSG_CANCEL_ACK, output.messages[0].type);
    
    tearDown();
}

/* Test: Flush Order Book */
void test_FlushOrderBook(void) {
    setUp();
    
    /* Add some orders */
    new_order_msg_t buy = {1, "TEST", 100, 50, SIDE_BUY, 1};
    new_order_msg_t sell = {2, "TEST", 105, 30, SIDE_SELL, 2};
    
    output_buffer_t out1, out2;
    output_buffer_init(&out1);
    output_buffer_init(&out2);
    order_book_add_order(&book, &buy, &out1);
    order_book_add_order(&book, &sell, &out2);
    
    /* Flush */
    order_book_flush(&book);
    
    /* Book should be empty */
    TEST_ASSERT_EQUAL(0, order_book_get_best_bid_price(&book));
    TEST_ASSERT_EQUAL(0, order_book_get_best_ask_price(&book));
    
    tearDown();
}

/* Test: Multiple Orders at Different Prices */
void test_MultipleOrdersAtDifferentPrices(void) {
    setUp();
    
    /* Build a book with depth */
    new_order_msg_t buy1 = {1, "TEST", 100, 50, SIDE_BUY, 1};
    new_order_msg_t buy2 = {1, "TEST", 99, 50, SIDE_BUY, 2};
    new_order_msg_t sell1 = {2, "TEST", 101, 50, SIDE_SELL, 3};
    new_order_msg_t sell2 = {2, "TEST", 102, 50, SIDE_SELL, 4};
    
    output_buffer_t out1, out2, out3, out4;
    output_buffer_init(&out1);
    output_buffer_init(&out2);
    output_buffer_init(&out3);
    output_buffer_init(&out4);
    
    order_book_add_order(&book, &buy1, &out1);
    order_book_add_order(&book, &buy2, &out2);
    order_book_add_order(&book, &sell1, &out3);
    order_book_add_order(&book, &sell2, &out4);
    
    /* Verify best prices */
    TEST_ASSERT_EQUAL(100, order_book_get_best_bid_price(&book));
    TEST_ASSERT_EQUAL(101, order_book_get_best_ask_price(&book));
    
    tearDown();
}

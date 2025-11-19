#include "unity.h"
#include <stdio.h>

/* External test function declarations */

/* test_message_formatter.c */
extern void test_FormatAck(void);
extern void test_FormatCancelAck(void);
extern void test_FormatTrade(void);
extern void test_FormatTopOfBookBid(void);
extern void test_FormatTopOfBookAsk(void);
extern void test_FormatTopOfBookEliminatedBid(void);
extern void test_FormatTopOfBookEliminatedAsk(void);

/* test_message_parser.c */
extern void test_ParseNewOrderBuy(void);
extern void test_ParseNewOrderSell(void);
extern void test_ParseMarketOrder(void);
extern void test_ParseCancel(void);
extern void test_ParseFlush(void);
extern void test_ParseComment(void);
extern void test_ParseBlankLine(void);
extern void test_ParseWithExtraWhitespace(void);
extern void test_ParseInvalidMessage(void);
extern void test_ParseInvalidNewOrder(void);

/* test_order_book.c */
extern void test_AddSingleBuyOrder(void);
extern void test_AddSingleSellOrder(void);
extern void test_MatchingBuyAndSell(void);
extern void test_PartialFill(void);
extern void test_MarketOrderBuy(void);
extern void test_MarketOrderSell(void);
extern void test_PriceTimePriority(void);
extern void test_CancelOrder(void);
extern void test_CancelNonExistentOrder(void);
extern void test_FlushOrderBook(void);
extern void test_MultipleOrdersAtDifferentPrices(void);

/* test_matching_engine.c */
extern void test_ProcessSingleOrder(void);
extern void test_MultipleSymbols(void);
extern void test_CancelOrderAcrossSymbols(void);
extern void test_FlushAllOrderBooks(void);
extern void test_IsolatedOrderBooks(void);
extern void test_CancelNonExistentOrderEngine(void);
extern void test_SameUserOrderIdDifferentSymbols(void);

/* test_scenarios.c */
extern void test_Scenario1_BalancedBook(void);
extern void test_Scenario3_ShallowAsk(void);
extern void test_Scenario9_MarketSellPartial(void);
extern void test_Scenario11_LimitSellPartial(void);
extern void test_Scenario13_MultipleOrdersAtBestPrice(void);
extern void test_Scenario15_CancelBehindBest(void);

/* Main test runner */
int main(void) {
    UNITY_BEGIN();
    
    printf("\n=== Message Formatter Tests ===\n");
    RUN_TEST(test_FormatAck);
    RUN_TEST(test_FormatCancelAck);
    RUN_TEST(test_FormatTrade);
    RUN_TEST(test_FormatTopOfBookBid);
    RUN_TEST(test_FormatTopOfBookAsk);
    RUN_TEST(test_FormatTopOfBookEliminatedBid);
    RUN_TEST(test_FormatTopOfBookEliminatedAsk);
    
    printf("\n=== Message Parser Tests ===\n");
    RUN_TEST(test_ParseNewOrderBuy);
    RUN_TEST(test_ParseNewOrderSell);
    RUN_TEST(test_ParseMarketOrder);
    RUN_TEST(test_ParseCancel);
    RUN_TEST(test_ParseFlush);
    RUN_TEST(test_ParseComment);
    RUN_TEST(test_ParseBlankLine);
    RUN_TEST(test_ParseWithExtraWhitespace);
    RUN_TEST(test_ParseInvalidMessage);
    RUN_TEST(test_ParseInvalidNewOrder);
    
    printf("\n=== Order Book Tests ===\n");
    RUN_TEST(test_AddSingleBuyOrder);
    RUN_TEST(test_AddSingleSellOrder);
    RUN_TEST(test_MatchingBuyAndSell);
    RUN_TEST(test_PartialFill);
    RUN_TEST(test_MarketOrderBuy);
    RUN_TEST(test_MarketOrderSell);
    RUN_TEST(test_PriceTimePriority);
    RUN_TEST(test_CancelOrder);
    RUN_TEST(test_CancelNonExistentOrder);
    RUN_TEST(test_FlushOrderBook);
    RUN_TEST(test_MultipleOrdersAtDifferentPrices);
    
    printf("\n=== Matching Engine Tests ===\n");
    RUN_TEST(test_ProcessSingleOrder);
    RUN_TEST(test_MultipleSymbols);
    RUN_TEST(test_CancelOrderAcrossSymbols);
    RUN_TEST(test_FlushAllOrderBooks);
    RUN_TEST(test_IsolatedOrderBooks);
    RUN_TEST(test_CancelNonExistentOrderEngine);
    RUN_TEST(test_SameUserOrderIdDifferentSymbols);
    
    printf("\n=== Scenario Tests ===\n");
    RUN_TEST(test_Scenario1_BalancedBook);
    RUN_TEST(test_Scenario3_ShallowAsk);
    RUN_TEST(test_Scenario9_MarketSellPartial);
    RUN_TEST(test_Scenario11_LimitSellPartial);
    RUN_TEST(test_Scenario13_MultipleOrdersAtBestPrice);
    RUN_TEST(test_Scenario15_CancelBehindBest);
    
    return UNITY_END();
}

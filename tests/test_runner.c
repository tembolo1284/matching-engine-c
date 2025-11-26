#include "unity.h"
#include <stdio.h>

/* ============================================================================
 * External Test Function Declarations
 * ============================================================================ */

/* ----------------------------------------------------------------------------
 * Message Formatter Tests (test_message_formatter.c)
 * ---------------------------------------------------------------------------- */
extern void test_FormatAck(void);
extern void test_FormatCancelAck(void);
extern void test_FormatTrade(void);
extern void test_FormatTopOfBookBid(void);
extern void test_FormatTopOfBookAsk(void);
extern void test_FormatTopOfBookEliminatedBid(void);
extern void test_FormatTopOfBookEliminatedAsk(void);

/* ----------------------------------------------------------------------------
 * Message Parser Tests (test_message_parser.c)
 * ---------------------------------------------------------------------------- */
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

/* ----------------------------------------------------------------------------
 * Order Book Tests (test_order_book.c)
 * ---------------------------------------------------------------------------- */
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

/* ----------------------------------------------------------------------------
 * Matching Engine Tests (test_matching_engine.c)
 * ---------------------------------------------------------------------------- */
extern void test_ProcessSingleOrder(void);
extern void test_MultipleSymbols(void);
extern void test_CancelOrderAcrossSymbols(void);
extern void test_FlushAllOrderBooks(void);
extern void test_IsolatedOrderBooks(void);
extern void test_CancelNonExistentOrderEngine(void);
extern void test_SameUserOrderIdDifferentSymbols(void);

/* ----------------------------------------------------------------------------
 * Scenario Tests (test_scenarios_odd.c, test_scenarios_even.c)
 * ---------------------------------------------------------------------------- */
extern void test_Scenario1_BalancedBook(void);
extern void test_Scenario3_ShallowAsk(void);
extern void test_Scenario9_MarketSellPartial(void);
extern void test_Scenario11_LimitSellPartial(void);
extern void test_Scenario13_MultipleOrdersAtBestPrice(void);
extern void test_Scenario15_CancelBehindBest(void);

/* ----------------------------------------------------------------------------
 * Memory Pool Tests (test_memory_pools.c) - NEW
 * ---------------------------------------------------------------------------- */
extern void test_MemoryPools_InitializeCorrectly(void);
extern void test_MemoryPools_TotalMemorySize(void);
extern void test_MemoryPools_OrdersAllocateFromPool(void);
extern void test_MemoryPools_CancelReturnsToPool(void);
extern void test_MemoryPools_FlushReturnsAllToPool(void);
extern void test_MemoryPools_TradeRemovesOrders(void);
extern void test_MemoryPools_PeakUsageTracking(void);
extern void test_MemoryPools_SharedByMultipleBooks(void);
extern void test_MemoryPools_HighVolumeOperations(void);
extern void test_MemoryPools_DataIntegrityUnderReuse(void);
extern void test_MemoryPools_EmptyPoolsZeroStats(void);
extern void test_MemoryPools_ReInitResets(void);

/* ----------------------------------------------------------------------------
 * Lock-Free Queue Tests (test_lockfree_queue.c) - NEW
 * ---------------------------------------------------------------------------- */
extern void test_Queue_InitializesEmpty(void);
extern void test_Queue_CorrectCapacity(void);
extern void test_Queue_StatsZeroedOnInit(void);
extern void test_Queue_SingleEnqueueDequeue(void);
extern void test_Queue_MultipleEnqueueDequeue(void);
extern void test_Queue_InterleavedOperations(void);
extern void test_Queue_FIFOOrder(void);
extern void test_Queue_DequeueFromEmptyFails(void);
extern void test_Queue_EnqueueToFullFails(void);
extern void test_Queue_WrapAround(void);
extern void test_Queue_EnqueueStatsTracked(void);
extern void test_Queue_DequeueStatsTracked(void);
extern void test_Queue_PeakSizeTracked(void);
extern void test_Queue_InvariantsHold(void);
extern void test_Queue_NullQueueHandling(void);
extern void test_Queue_NullItemHandling(void);
extern void test_Queue_DataIntegrity(void);
extern void test_Queue_SizeConsistency(void);

/* ----------------------------------------------------------------------------
 * Symbol Router Tests (test_symbol_router.c) - NEW
 * ---------------------------------------------------------------------------- */
extern void test_SymbolRouter_AtoM_RoutesToProcessor0(void);
extern void test_SymbolRouter_NtoZ_RoutesToProcessor1(void);
extern void test_SymbolRouter_M_BoundaryProcessor0(void);
extern void test_SymbolRouter_N_BoundaryProcessor1(void);
extern void test_SymbolRouter_A_StartBoundary(void);
extern void test_SymbolRouter_Z_EndBoundary(void);
extern void test_SymbolRouter_Lowercase_AtoM(void);
extern void test_SymbolRouter_Lowercase_NtoZ(void);
extern void test_SymbolRouter_MixedCase(void);
extern void test_SymbolRouter_NullSymbol(void);
extern void test_SymbolRouter_EmptySymbol(void);
extern void test_SymbolRouter_NumericSymbol(void);
extern void test_SymbolRouter_SpecialCharSymbol(void);
extern void test_SymbolRouter_SingleCharSymbols(void);
extern void test_SymbolIsValid_ValidSymbols(void);
extern void test_SymbolIsValid_InvalidSymbols(void);
extern void test_GetProcessorName(void);
extern void test_SymbolRouter_Consistency(void);
extern void test_SymbolRouter_ValidProcessorIds(void);

/* ============================================================================
 * Main Test Runner
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();
    
    /* ========================================================================
     * Original Tests
     * ======================================================================== */
    
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
    
    /* ========================================================================
     * NEW Tests - Memory Pools, Lock-Free Queue, Symbol Router
     * ======================================================================== */
    
    printf("\n=== Memory Pool Tests ===\n");
    RUN_TEST(test_MemoryPools_InitializeCorrectly);
    RUN_TEST(test_MemoryPools_TotalMemorySize);
    RUN_TEST(test_MemoryPools_OrdersAllocateFromPool);
    RUN_TEST(test_MemoryPools_CancelReturnsToPool);
    RUN_TEST(test_MemoryPools_FlushReturnsAllToPool);
    RUN_TEST(test_MemoryPools_TradeRemovesOrders);
    RUN_TEST(test_MemoryPools_PeakUsageTracking);
    RUN_TEST(test_MemoryPools_SharedByMultipleBooks);
    RUN_TEST(test_MemoryPools_HighVolumeOperations);
    RUN_TEST(test_MemoryPools_DataIntegrityUnderReuse);
    RUN_TEST(test_MemoryPools_EmptyPoolsZeroStats);
    RUN_TEST(test_MemoryPools_ReInitResets);
    
    printf("\n=== Lock-Free Queue Tests ===\n");
    RUN_TEST(test_Queue_InitializesEmpty);
    RUN_TEST(test_Queue_CorrectCapacity);
    RUN_TEST(test_Queue_StatsZeroedOnInit);
    RUN_TEST(test_Queue_SingleEnqueueDequeue);
    RUN_TEST(test_Queue_MultipleEnqueueDequeue);
    RUN_TEST(test_Queue_InterleavedOperations);
    RUN_TEST(test_Queue_FIFOOrder);
    RUN_TEST(test_Queue_DequeueFromEmptyFails);
    RUN_TEST(test_Queue_EnqueueToFullFails);
    RUN_TEST(test_Queue_WrapAround);
    RUN_TEST(test_Queue_EnqueueStatsTracked);
    RUN_TEST(test_Queue_DequeueStatsTracked);
    RUN_TEST(test_Queue_PeakSizeTracked);
    RUN_TEST(test_Queue_InvariantsHold);
    RUN_TEST(test_Queue_NullQueueHandling);
    RUN_TEST(test_Queue_NullItemHandling);
    RUN_TEST(test_Queue_DataIntegrity);
    RUN_TEST(test_Queue_SizeConsistency);
    
    printf("\n=== Symbol Router Tests ===\n");
    RUN_TEST(test_SymbolRouter_AtoM_RoutesToProcessor0);
    RUN_TEST(test_SymbolRouter_NtoZ_RoutesToProcessor1);
    RUN_TEST(test_SymbolRouter_M_BoundaryProcessor0);
    RUN_TEST(test_SymbolRouter_N_BoundaryProcessor1);
    RUN_TEST(test_SymbolRouter_A_StartBoundary);
    RUN_TEST(test_SymbolRouter_Z_EndBoundary);
    RUN_TEST(test_SymbolRouter_Lowercase_AtoM);
    RUN_TEST(test_SymbolRouter_Lowercase_NtoZ);
    RUN_TEST(test_SymbolRouter_MixedCase);
    RUN_TEST(test_SymbolRouter_NullSymbol);
    RUN_TEST(test_SymbolRouter_EmptySymbol);
    RUN_TEST(test_SymbolRouter_NumericSymbol);
    RUN_TEST(test_SymbolRouter_SpecialCharSymbol);
    RUN_TEST(test_SymbolRouter_SingleCharSymbols);
    RUN_TEST(test_SymbolIsValid_ValidSymbols);
    RUN_TEST(test_SymbolIsValid_InvalidSymbols);
    RUN_TEST(test_GetProcessorName);
    RUN_TEST(test_SymbolRouter_Consistency);
    RUN_TEST(test_SymbolRouter_ValidProcessorIds);
    
    return UNITY_END();
}

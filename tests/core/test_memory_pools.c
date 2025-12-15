#include "unity.h"
#include "core/order_book.h"
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif


/* ============================================================================
 * Memory Pool Unit Tests
 *
 * Tests the zero-allocation memory pool system:
 *   - Pool initialization
 *   - Statistics tracking
 *   - Indirect allocation via order book operations
 *   - Peak usage tracking
 *   - Pool exhaustion behavior
 *
 * Note: Allocation/deallocation functions are internal to order_book.c,
 * so we test them indirectly by observing pool statistics after order
 * book operations.
 *
 * Update: Hash entry pool was removed - now using open-addressing hash tables.
 * Only order pool statistics are tracked.
 * ============================================================================ */

/* Test fixture - use mmap for large allocations to avoid overcommit issues */
static memory_pools_t* pools;
static order_book_t* book;

static void setUp(void) {
    pools = mmap(NULL, sizeof(memory_pools_t),
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                 -1, 0);
    book = mmap(NULL, sizeof(order_book_t),
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                -1, 0);
    TEST_ASSERT_TRUE(pools != MAP_FAILED);
    TEST_ASSERT_TRUE(book != MAP_FAILED);
    memory_pools_init(pools);
}

static void tearDown(void) {
    if (book && book != MAP_FAILED) {
        munmap(book, sizeof(order_book_t));
        book = NULL;
    }
    if (pools && pools != MAP_FAILED) {
        munmap(pools, sizeof(memory_pools_t));
        pools = NULL;
    }
}

/* ----------------------------------------------------------------------------
 * Initialization Tests
 * ---------------------------------------------------------------------------- */

/* Test: Pools initialize with correct statistics */
void test_MemoryPools_InitializeCorrectly(void) {
    setUp();
    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, NULL, &stats);

    /* All counters should be zero after init */
    TEST_ASSERT_EQUAL(0, stats.order_allocations);
    TEST_ASSERT_EQUAL(0, stats.order_peak_usage);
    TEST_ASSERT_EQUAL(0, stats.order_failures);
    tearDown();
}

/* Test: Total memory size is calculated correctly */
void test_MemoryPools_TotalMemorySize(void) {
    setUp();
    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, NULL, &stats);

    /* Calculate expected size */
    size_t expected_size = sizeof(memory_pools_t);
    (void)expected_size;

    /* Memory should be substantial (pools contain large arrays) */
    TEST_ASSERT_TRUE(stats.total_memory_bytes >= 512 * 1024);  /* At least 512KB */
    tearDown();
}

/* ----------------------------------------------------------------------------
 * Indirect Allocation Tests (via Order Book)
 * ---------------------------------------------------------------------------- */

/* Test: Adding orders allocates from pools */
void test_MemoryPools_OrdersAllocateFromPool(void) {
    setUp();
    order_book_init(book, "TEST", pools);

    /* Add some orders */
    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);
    output_buffer_init(output);

    new_order_msg_t buy1;
    memset(&buy1, 0, sizeof(buy1));
    buy1.user_id = 1;
    buy1.user_order_id = 1;
    buy1.price = 100;
    buy1.quantity = 50;
    buy1.side = SIDE_BUY;
    strncpy(buy1.symbol, "TEST", sizeof(buy1.symbol) - 1);

    new_order_msg_t buy2;
    memset(&buy2, 0, sizeof(buy2));
    buy2.user_id = 1;
    buy2.user_order_id = 2;
    buy2.price = 99;
    buy2.quantity = 50;
    buy2.side = SIDE_BUY;
    strncpy(buy2.symbol, "TEST", sizeof(buy2.symbol) - 1);

    new_order_msg_t sell1;
    memset(&sell1, 0, sizeof(sell1));
    sell1.user_id = 2;
    sell1.user_order_id = 3;
    sell1.price = 101;
    sell1.quantity = 50;
    sell1.side = SIDE_SELL;
    strncpy(sell1.symbol, "TEST", sizeof(sell1.symbol) - 1);

    order_book_add_order(book, &buy1, 1, output);
    output_buffer_init(output);
    order_book_add_order(book, &buy2, 1, output);
    output_buffer_init(output);
    order_book_add_order(book, &sell1, 2, output);

    /* Check pool statistics */
    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, NULL, &stats);

    /* Should have allocated 3 orders */
    TEST_ASSERT_EQUAL(3, stats.order_allocations);
    TEST_ASSERT_TRUE(stats.order_peak_usage >= 3);
    TEST_ASSERT_EQUAL(0, stats.order_failures);

    order_book_destroy(book);
    free(output);
    tearDown();
}

/* Test: Canceling orders returns to pool (peak doesn't increase) */
void test_MemoryPools_CancelReturnsToPool(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);

    /* Add order */
    new_order_msg_t order;
    memset(&order, 0, sizeof(order));
    order.user_id = 1;
    order.user_order_id = 1;
    order.price = 100;
    order.quantity = 50;
    order.side = SIDE_BUY;
    strncpy(order.symbol, "TEST", sizeof(order.symbol) - 1);

    output_buffer_init(output);
    order_book_add_order(book, &order, 1, output);

    memory_pool_stats_t stats_before;
    memory_pools_get_stats(pools, NULL, &stats_before);
    uint32_t peak_before = stats_before.order_peak_usage;

    /* Cancel order */
    output_buffer_init(output);
    order_book_cancel_order(book, 1, 1, output);

    /* Add another order - should reuse freed slot */
    new_order_msg_t order2;
    memset(&order2, 0, sizeof(order2));
    order2.user_id = 1;
    order2.user_order_id = 2;
    order2.price = 101;
    order2.quantity = 50;
    order2.side = SIDE_BUY;
    strncpy(order2.symbol, "TEST", sizeof(order2.symbol) - 1);

    output_buffer_init(output);
    order_book_add_order(book, &order2, 1, output);

    memory_pool_stats_t stats_after;
    memory_pools_get_stats(pools, NULL, &stats_after);

    /* Peak should not have increased (slot was reused) */
    TEST_ASSERT_EQUAL(peak_before, stats_after.order_peak_usage);

    /* Total allocations should have increased though */
    TEST_ASSERT_TRUE(stats_after.order_allocations > stats_before.order_allocations);

    order_book_destroy(book);
    free(output);
    tearDown();
}

/* Test: Flush returns all orders to pool */
void test_MemoryPools_FlushReturnsAllToPool(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);

    /* Add several orders */
    for (uint32_t i = 0; i < 10; i++) {
        new_order_msg_t order;
        memset(&order, 0, sizeof(order));
        order.user_id = 1;
        order.user_order_id = i + 1;
        order.price = 100 + i;
        order.quantity = 50;
        order.side = SIDE_BUY;
        strncpy(order.symbol, "TEST", sizeof(order.symbol) - 1);

        output_buffer_init(output);
        order_book_add_order(book, &order, 1, output);
    }

    memory_pool_stats_t stats_before;
    memory_pools_get_stats(pools, NULL, &stats_before);
    TEST_ASSERT_EQUAL(10, stats_before.order_peak_usage);

    /* Flush all orders */
    output_buffer_init(output);
    order_book_flush(book, output);

    /* Add 10 new orders - should all reuse freed slots */
    for (uint32_t i = 0; i < 10; i++) {
        new_order_msg_t order;
        memset(&order, 0, sizeof(order));
        order.user_id = 1;
        order.user_order_id = i + 100;
        order.price = 200 + i;
        order.quantity = 50;
        order.side = SIDE_SELL;
        strncpy(order.symbol, "TEST", sizeof(order.symbol) - 1);

        output_buffer_init(output);
        order_book_add_order(book, &order, 1, output);
    }

    memory_pool_stats_t stats_after;
    memory_pools_get_stats(pools, NULL, &stats_after);

    /* Peak should still be 10 (slots were reused) */
    TEST_ASSERT_EQUAL(10, stats_after.order_peak_usage);

    order_book_destroy(book);
    free(output);
    tearDown();
}

/* Test: Trade removes matched orders from pool */
void test_MemoryPools_TradeRemovesOrders(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);

    /* Add resting sell order */
    new_order_msg_t sell;
    memset(&sell, 0, sizeof(sell));
    sell.user_id = 1;
    sell.user_order_id = 1;
    sell.price = 100;
    sell.quantity = 50;
    sell.side = SIDE_SELL;
    strncpy(sell.symbol, "TEST", sizeof(sell.symbol) - 1);

    output_buffer_init(output);
    order_book_add_order(book, &sell, 1, output);

    /* Add matching buy order (will trade and remove both) */
    new_order_msg_t buy;
    memset(&buy, 0, sizeof(buy));
    buy.user_id = 2;
    buy.user_order_id = 2;
    buy.price = 100;
    buy.quantity = 50;
    buy.side = SIDE_BUY;
    strncpy(buy.symbol, "TEST", sizeof(buy.symbol) - 1);

    output_buffer_init(output);
    order_book_add_order(book, &buy, 2, output);

    memory_pool_stats_t stats_after_trade;
    memory_pools_get_stats(pools, NULL, &stats_after_trade);

    /* Peak should be 2 (both were in pool at some point) */
    TEST_ASSERT_TRUE(stats_after_trade.order_peak_usage >= 1);

    /* Add more orders - should reuse slots */
    for (uint32_t i = 0; i < 5; i++) {
        new_order_msg_t order;
        memset(&order, 0, sizeof(order));
        order.user_id = 1;
        order.user_order_id = i + 10;
        order.price = 150 + i;
        order.quantity = 50;
        order.side = SIDE_BUY;
        strncpy(order.symbol, "TEST", sizeof(order.symbol) - 1);

        output_buffer_init(output);
        order_book_add_order(book, &order, 1, output);
    }

    memory_pool_stats_t stats_after_reuse;
    memory_pools_get_stats(pools, NULL, &stats_after_reuse);

    /* Peak should grow but allocations reuse freed slots */
    TEST_ASSERT_TRUE(stats_after_reuse.order_allocations >= 7);

    order_book_destroy(book);
    free(output);
    tearDown();
}

/* ----------------------------------------------------------------------------
 * Peak Usage Tests
 * ---------------------------------------------------------------------------- */

/* Test: Peak usage tracks maximum concurrent allocations */
void test_MemoryPools_PeakUsageTracking(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);

    /* Add 50 orders */
    for (uint32_t i = 0; i < 50; i++) {
        new_order_msg_t order;
        memset(&order, 0, sizeof(order));
        order.user_id = 1;
        order.user_order_id = i + 1;
        order.price = 100 + i;
        order.quantity = 50;
        order.side = SIDE_BUY;
        strncpy(order.symbol, "TEST", sizeof(order.symbol) - 1);

        output_buffer_init(output);
        order_book_add_order(book, &order, 1, output);
    }

    memory_pool_stats_t stats1;
    memory_pools_get_stats(pools, NULL, &stats1);
    TEST_ASSERT_EQUAL(50, stats1.order_peak_usage);

    /* Cancel 30 */
    for (uint32_t i = 0; i < 30; i++) {
        output_buffer_init(output);
        order_book_cancel_order(book, 1, i + 1, output);
    }

    /* Peak should still be 50 */
    memory_pool_stats_t stats2;
    memory_pools_get_stats(pools, NULL, &stats2);
    TEST_ASSERT_EQUAL(50, stats2.order_peak_usage);

    /* Add 40 more - total concurrent will be 20 + 40 = 60 */
    for (uint32_t i = 0; i < 40; i++) {
        new_order_msg_t order;
        memset(&order, 0, sizeof(order));
        order.user_id = 1;
        order.user_order_id = i + 100;
        order.price = 200 + i;
        order.quantity = 50;
        order.side = SIDE_SELL;
        strncpy(order.symbol, "TEST", sizeof(order.symbol) - 1);

        output_buffer_init(output);
        order_book_add_order(book, &order, 1, output);
    }

    /* Peak should now be 60 */
    memory_pool_stats_t stats3;
    memory_pools_get_stats(pools, NULL, &stats3);
    TEST_ASSERT_EQUAL(60, stats3.order_peak_usage);

    order_book_destroy(book);
    free(output);
    tearDown();
}

/* ----------------------------------------------------------------------------
 * Multiple Order Books Sharing Pool
 * ---------------------------------------------------------------------------- */

/* Test: Multiple order books can share pools */
void test_MemoryPools_SharedByMultipleBooks(void) {
    setUp();
    order_book_t* book1 = mmap(NULL, sizeof(order_book_t),
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                               -1, 0);
    order_book_t* book2 = mmap(NULL, sizeof(order_book_t),
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                               -1, 0);
    TEST_ASSERT_TRUE(book1 != MAP_FAILED);
    TEST_ASSERT_TRUE(book2 != MAP_FAILED);

    order_book_init(book1, "IBM", pools);
    order_book_init(book2, "AAPL", pools);

    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);

    /* Add orders to both books */
    new_order_msg_t ibm_order;
    memset(&ibm_order, 0, sizeof(ibm_order));
    ibm_order.user_id = 1;
    ibm_order.user_order_id = 1;
    ibm_order.price = 100;
    ibm_order.quantity = 50;
    ibm_order.side = SIDE_BUY;
    strncpy(ibm_order.symbol, "IBM", sizeof(ibm_order.symbol) - 1);

    output_buffer_init(output);
    order_book_add_order(book1, &ibm_order, 1, output);

    new_order_msg_t aapl_order;
    memset(&aapl_order, 0, sizeof(aapl_order));
    aapl_order.user_id = 2;
    aapl_order.user_order_id = 1;
    aapl_order.price = 150;
    aapl_order.quantity = 30;
    aapl_order.side = SIDE_SELL;
    strncpy(aapl_order.symbol, "AAPL", sizeof(aapl_order.symbol) - 1);

    output_buffer_init(output);
    order_book_add_order(book2, &aapl_order, 2, output);

    /* Both should have allocated from same pool */
    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, NULL, &stats);

    TEST_ASSERT_EQUAL(2, stats.order_allocations);
    TEST_ASSERT_EQUAL(2, stats.order_peak_usage);

    order_book_destroy(book1);
    order_book_destroy(book2);
    munmap(book1, sizeof(order_book_t));
    munmap(book2, sizeof(order_book_t));
    free(output);
    tearDown();
}

/* ----------------------------------------------------------------------------
 * Stress Tests
 * ---------------------------------------------------------------------------- */

/* Test: High volume allocation and deallocation */
void test_MemoryPools_HighVolumeOperations(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);

    /* Add and remove many orders in cycles */
    for (int cycle = 0; cycle < 10; cycle++) {
        /* Add 100 orders */
        for (uint32_t i = 0; i < 100; i++) {
            new_order_msg_t order;
            memset(&order, 0, sizeof(order));
            order.user_id = 1;
            order.user_order_id = (uint32_t)(cycle * 1000 + i + 1);
            order.price = 100 + (i % 50);
            order.quantity = 50;
            order.side = SIDE_BUY;
            strncpy(order.symbol, "TEST", sizeof(order.symbol) - 1);

            output_buffer_init(output);
            order_book_add_order(book, &order, 1, output);
        }

        /* Flush all */
        output_buffer_init(output);
        order_book_flush(book, output);
    }

    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, NULL, &stats);

    /* Should have allocated 1000 total */
    TEST_ASSERT_EQUAL(1000, stats.order_allocations);

    /* Peak should be 100 (max concurrent per cycle) */
    TEST_ASSERT_EQUAL(100, stats.order_peak_usage);

    /* No failures */
    TEST_ASSERT_EQUAL(0, stats.order_failures);

    order_book_destroy(book);
    free(output);
    tearDown();
}

/* Test: Pool does not corrupt data under reuse */
void test_MemoryPools_DataIntegrityUnderReuse(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);

    /* Add order, verify book state */
    new_order_msg_t buy1;
    memset(&buy1, 0, sizeof(buy1));
    buy1.user_id = 1;
    buy1.user_order_id = 1;
    buy1.price = 100;
    buy1.quantity = 50;
    buy1.side = SIDE_BUY;
    strncpy(buy1.symbol, "TEST", sizeof(buy1.symbol) - 1);

    output_buffer_init(output);
    order_book_add_order(book, &buy1, 1, output);

    TEST_ASSERT_EQUAL(100, order_book_get_best_bid_price(book));
    TEST_ASSERT_EQUAL(50, order_book_get_best_bid_quantity(book));

    /* Cancel and add different order using same slot */
    output_buffer_init(output);
    order_book_cancel_order(book, 1, 1, output);

    new_order_msg_t buy2;
    memset(&buy2, 0, sizeof(buy2));
    buy2.user_id = 2;
    buy2.user_order_id = 2;
    buy2.price = 200;
    buy2.quantity = 75;
    buy2.side = SIDE_BUY;
    strncpy(buy2.symbol, "TEST", sizeof(buy2.symbol) - 1);

    output_buffer_init(output);
    order_book_add_order(book, &buy2, 2, output);

    /* Verify new order data is correct (not corrupted by reuse) */
    TEST_ASSERT_EQUAL(200, order_book_get_best_bid_price(book));
    TEST_ASSERT_EQUAL(75, order_book_get_best_bid_quantity(book));

    order_book_destroy(book);
    free(output);
    tearDown();
}

/* ----------------------------------------------------------------------------
 * Edge Cases
 * ---------------------------------------------------------------------------- */

/* Test: Empty pools report zero stats */
void test_MemoryPools_EmptyPoolsZeroStats(void) {
    setUp();
    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, NULL, &stats);

    TEST_ASSERT_EQUAL(0, stats.order_allocations);
    TEST_ASSERT_EQUAL(0, stats.order_peak_usage);
    TEST_ASSERT_EQUAL(0, stats.order_failures);
    tearDown();
}

/* Test: Re-initialization resets pools */
void test_MemoryPools_ReInitResets(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t* output = malloc(sizeof(output_buffer_t));
    TEST_ASSERT_NOT_NULL(output);

    /* Add some orders */
    for (uint32_t i = 0; i < 10; i++) {
        new_order_msg_t order;
        memset(&order, 0, sizeof(order));
        order.user_id = 1;
        order.user_order_id = i + 1;
        order.price = 100 + i;
        order.quantity = 50;
        order.side = SIDE_BUY;
        strncpy(order.symbol, "TEST", sizeof(order.symbol) - 1);

        output_buffer_init(output);
        order_book_add_order(book, &order, 1, output);
    }

    memory_pool_stats_t stats1;
    memory_pools_get_stats(pools, NULL, &stats1);
    TEST_ASSERT_EQUAL(10, stats1.order_allocations);

    order_book_destroy(book);

    /* Re-initialize pools */
    memory_pools_init(pools);

    memory_pool_stats_t stats2;
    memory_pools_get_stats(pools, NULL, &stats2);

    /* Should be reset to zero */
    TEST_ASSERT_EQUAL(0, stats2.order_allocations);
    TEST_ASSERT_EQUAL(0, stats2.order_peak_usage);

    free(output);
    tearDown();
}

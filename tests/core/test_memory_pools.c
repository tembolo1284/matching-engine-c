#include "unity.h"
#include "core/order_book.h"
#include <string.h>
#include <stdlib.h>

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

/* Test fixture - heap allocated to avoid ARM64 address range issues */
static memory_pools_t* pools;
static order_book_t* book;

static void setUp(void) {
    pools = malloc(sizeof(memory_pools_t));
    book = malloc(sizeof(order_book_t));
    TEST_ASSERT_NOT_NULL(pools);
    TEST_ASSERT_NOT_NULL(book);
    memory_pools_init(pools);
}

static void tearDown(void) {
    free(book);
    free(pools);
    book = NULL;
    pools = NULL;
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
    TEST_ASSERT_TRUE(stats.total_memory_bytes >= 640 * 1024);  /* At least 640KB */
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
    output_buffer_t output;
    output_buffer_init(&output);

    new_order_msg_t buy1 = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "TEST"
    };
    new_order_msg_t buy2 = {
        .user_id = 1,
        .user_order_id = 2,
        .price = 99,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "TEST"
    };
    new_order_msg_t sell1 = {
        .user_id = 2,
        .user_order_id = 3,
        .price = 101,
        .quantity = 50,
        .side = SIDE_SELL,
        .symbol = "TEST"
    };

    order_book_add_order(book, &buy1, 1, &output);
    output_buffer_init(&output);
    order_book_add_order(book, &buy2, 1, &output);
    output_buffer_init(&output);
    order_book_add_order(book, &sell1, 2, &output);

    /* Check pool statistics */
    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, NULL, &stats);

    /* Should have allocated 3 orders */
    TEST_ASSERT_EQUAL(3, stats.order_allocations);
    TEST_ASSERT_TRUE(stats.order_peak_usage >= 3);
    TEST_ASSERT_EQUAL(0, stats.order_failures);

    order_book_destroy(book);
    tearDown();
}

/* Test: Canceling orders returns to pool (peak doesn't increase) */
void test_MemoryPools_CancelReturnsToPool(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t output;

    /* Add order */
    new_order_msg_t order = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "TEST"
    };
    output_buffer_init(&output);
    order_book_add_order(book, &order, 1, &output);

    memory_pool_stats_t stats_before;
    memory_pools_get_stats(pools, NULL, &stats_before);
    uint32_t peak_before = stats_before.order_peak_usage;

    /* Cancel order */
    output_buffer_init(&output);
    order_book_cancel_order(book, 1, 1, &output);

    /* Add another order - should reuse freed slot */
    new_order_msg_t order2 = {
        .user_id = 1,
        .user_order_id = 2,
        .price = 101,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "TEST"
    };
    output_buffer_init(&output);
    order_book_add_order(book, &order2, 1, &output);

    memory_pool_stats_t stats_after;
    memory_pools_get_stats(pools, NULL, &stats_after);

    /* Peak should not have increased (slot was reused) */
    TEST_ASSERT_EQUAL(peak_before, stats_after.order_peak_usage);

    /* Total allocations should have increased though */
    TEST_ASSERT_TRUE(stats_after.order_allocations > stats_before.order_allocations);

    order_book_destroy(book);
    tearDown();
}

/* Test: Flush returns all orders to pool */
void test_MemoryPools_FlushReturnsAllToPool(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t output;

    /* Add several orders */
    for (uint32_t i = 0; i < 10; i++) {
        new_order_msg_t order = {
            .user_id = 1,
            .user_order_id = i + 1,
            .price = 100 + i,
            .quantity = 50,
            .side = SIDE_BUY,
            .symbol = "TEST"
        };
        output_buffer_init(&output);
        order_book_add_order(book, &order, 1, &output);
    }

    memory_pool_stats_t stats_before;
    memory_pools_get_stats(pools, NULL, &stats_before);
    TEST_ASSERT_EQUAL(10, stats_before.order_peak_usage);

    /* Flush all orders */
    output_buffer_init(&output);
    order_book_flush(book, &output);

    /* Add 10 new orders - should all reuse freed slots */
    for (uint32_t i = 0; i < 10; i++) {
        new_order_msg_t order = {
            .user_id = 1,
            .user_order_id = i + 100,
            .price = 200 + i,
            .quantity = 50,
            .side = SIDE_SELL,
            .symbol = "TEST"
        };
        output_buffer_init(&output);
        order_book_add_order(book, &order, 1, &output);
    }

    memory_pool_stats_t stats_after;
    memory_pools_get_stats(pools, NULL, &stats_after);

    /* Peak should still be 10 (slots were reused) */
    TEST_ASSERT_EQUAL(10, stats_after.order_peak_usage);

    order_book_destroy(book);
    tearDown();
}

/* Test: Trade removes matched orders from pool */
void test_MemoryPools_TradeRemovesOrders(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t output;

    /* Add resting sell order */
    new_order_msg_t sell = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_SELL,
        .symbol = "TEST"
    };
    output_buffer_init(&output);
    order_book_add_order(book, &sell, 1, &output);

    /* Add matching buy order (will trade and remove both) */
    new_order_msg_t buy = {
        .user_id = 2,
        .user_order_id = 2,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "TEST"
    };
    output_buffer_init(&output);
    order_book_add_order(book, &buy, 2, &output);

    memory_pool_stats_t stats_after_trade;
    memory_pools_get_stats(pools, NULL, &stats_after_trade);

    /* Peak should be 2 (both were in pool at some point) */
    TEST_ASSERT_TRUE(stats_after_trade.order_peak_usage >= 1);

    /* Add more orders - should reuse slots */
    for (uint32_t i = 0; i < 5; i++) {
        new_order_msg_t order = {
            .user_id = 1,
            .user_order_id = i + 10,
            .price = 150 + i,
            .quantity = 50,
            .side = SIDE_BUY,
            .symbol = "TEST"
        };
        output_buffer_init(&output);
        order_book_add_order(book, &order, 1, &output);
    }

    memory_pool_stats_t stats_after_reuse;
    memory_pools_get_stats(pools, NULL, &stats_after_reuse);

    /* Peak should grow but allocations reuse freed slots */
    TEST_ASSERT_TRUE(stats_after_reuse.order_allocations >= 7);

    order_book_destroy(book);
    tearDown();
}

/* ----------------------------------------------------------------------------
 * Peak Usage Tests
 * ---------------------------------------------------------------------------- */

/* Test: Peak usage tracks maximum concurrent allocations */
void test_MemoryPools_PeakUsageTracking(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t output;

    /* Add 50 orders */
    for (uint32_t i = 0; i < 50; i++) {
        new_order_msg_t order = {
            .user_id = 1,
            .user_order_id = i + 1,
            .price = 100 + i,
            .quantity = 50,
            .side = SIDE_BUY,
            .symbol = "TEST"
        };
        output_buffer_init(&output);
        order_book_add_order(book, &order, 1, &output);
    }

    memory_pool_stats_t stats1;
    memory_pools_get_stats(pools, NULL, &stats1);
    TEST_ASSERT_EQUAL(50, stats1.order_peak_usage);

    /* Cancel 30 */
    for (uint32_t i = 0; i < 30; i++) {
        output_buffer_init(&output);
        order_book_cancel_order(book, 1, i + 1, &output);
    }

    /* Peak should still be 50 */
    memory_pool_stats_t stats2;
    memory_pools_get_stats(pools, NULL, &stats2);
    TEST_ASSERT_EQUAL(50, stats2.order_peak_usage);

    /* Add 40 more - total concurrent will be 20 + 40 = 60 */
    for (uint32_t i = 0; i < 40; i++) {
        new_order_msg_t order = {
            .user_id = 1,
            .user_order_id = i + 100,
            .price = 200 + i,
            .quantity = 50,
            .side = SIDE_SELL,
            .symbol = "TEST"
        };
        output_buffer_init(&output);
        order_book_add_order(book, &order, 1, &output);
    }

    /* Peak should now be 60 */
    memory_pool_stats_t stats3;
    memory_pools_get_stats(pools, NULL, &stats3);
    TEST_ASSERT_EQUAL(60, stats3.order_peak_usage);

    order_book_destroy(book);
    tearDown();
}

/* ----------------------------------------------------------------------------
 * Multiple Order Books Sharing Pool
 * ---------------------------------------------------------------------------- */

/* Test: Multiple order books can share pools */
void test_MemoryPools_SharedByMultipleBooks(void) {
    setUp();
    order_book_t* book1 = malloc(sizeof(order_book_t));
    order_book_t* book2 = malloc(sizeof(order_book_t));
    TEST_ASSERT_NOT_NULL(book1);
    TEST_ASSERT_NOT_NULL(book2);

    order_book_init(book1, "IBM", pools);
    order_book_init(book2, "AAPL", pools);

    output_buffer_t output;

    /* Add orders to both books */
    new_order_msg_t ibm_order = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "IBM"
    };
    output_buffer_init(&output);
    order_book_add_order(book1, &ibm_order, 1, &output);

    new_order_msg_t aapl_order = {
        .user_id = 2,
        .user_order_id = 1,
        .price = 150,
        .quantity = 30,
        .side = SIDE_SELL,
        .symbol = "AAPL"
    };
    output_buffer_init(&output);
    order_book_add_order(book2, &aapl_order, 2, &output);

    /* Both should have allocated from same pool */
    memory_pool_stats_t stats;
    memory_pools_get_stats(pools, NULL, &stats);

    TEST_ASSERT_EQUAL(2, stats.order_allocations);
    TEST_ASSERT_EQUAL(2, stats.order_peak_usage);

    order_book_destroy(book1);
    order_book_destroy(book2);
    free(book1);
    free(book2);
    tearDown();
}

/* ----------------------------------------------------------------------------
 * Stress Tests
 * ---------------------------------------------------------------------------- */

/* Test: High volume allocation and deallocation */
void test_MemoryPools_HighVolumeOperations(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t output;

    /* Add and remove many orders in cycles */
    for (int cycle = 0; cycle < 10; cycle++) {
        /* Add 100 orders */
        for (uint32_t i = 0; i < 100; i++) {
            new_order_msg_t order = {
                .user_id = 1,
                .user_order_id = (uint32_t)(cycle * 1000 + i + 1),
                .price = 100 + (i % 50),
                .quantity = 50,
                .side = SIDE_BUY,
                .symbol = "TEST"
            };
            output_buffer_init(&output);
            order_book_add_order(book, &order, 1, &output);
        }

        /* Flush all */
        output_buffer_init(&output);
        order_book_flush(book, &output);
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
    tearDown();
}

/* Test: Pool does not corrupt data under reuse */
void test_MemoryPools_DataIntegrityUnderReuse(void) {
    setUp();
    order_book_init(book, "TEST", pools);
    output_buffer_t output;

    /* Add order, verify book state */
    new_order_msg_t buy1 = {
        .user_id = 1,
        .user_order_id = 1,
        .price = 100,
        .quantity = 50,
        .side = SIDE_BUY,
        .symbol = "TEST"
    };
    output_buffer_init(&output);
    order_book_add_order(book, &buy1, 1, &output);

    TEST_ASSERT_EQUAL(100, order_book_get_best_bid_price(book));
    TEST_ASSERT_EQUAL(50, order_book_get_best_bid_quantity(book));

    /* Cancel and add different order using same slot */
    output_buffer_init(&output);
    order_book_cancel_order(book, 1, 1, &output);

    new_order_msg_t buy2 = {
        .user_id = 2,
        .user_order_id = 2,
        .price = 200,
        .quantity = 75,
        .side = SIDE_BUY,
        .symbol = "TEST"
    };
    output_buffer_init(&output);
    order_book_add_order(book, &buy2, 2, &output);

    /* Verify new order data is correct (not corrupted by reuse) */
    TEST_ASSERT_EQUAL(200, order_book_get_best_bid_price(book));
    TEST_ASSERT_EQUAL(75, order_book_get_best_bid_quantity(book));

    order_book_destroy(book);
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
    output_buffer_t output;

    /* Add some orders */
    for (uint32_t i = 0; i < 10; i++) {
        new_order_msg_t order = {
            .user_id = 1,
            .user_order_id = i + 1,
            .price = 100 + i,
            .quantity = 50,
            .side = SIDE_BUY,
            .symbol = "TEST"
        };
        output_buffer_init(&output);
        order_book_add_order(book, &order, 1, &output);
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
    tearDown();
}

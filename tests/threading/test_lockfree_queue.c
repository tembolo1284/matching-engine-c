#include "unity.h"
#include "threading/lockfree_queue.h"
#include <string.h>

/* ============================================================================
 * Lock-Free Queue Unit Tests
 *
 * Tests the SPSC (Single-Producer Single-Consumer) lock-free queue:
 *   - Basic enqueue/dequeue operations
 *   - FIFO ordering guarantee
 *   - Empty/full boundary conditions
 *   - Size and capacity tracking
 *   - Statistics collection
 *   - Batch dequeue operations
 *   - Invariant verification
 * ============================================================================ */

/* ----------------------------------------------------------------------------
 * Test Queue Type Declaration
 * ---------------------------------------------------------------------------- */

/* Simple test item type */
typedef struct {
    uint32_t id;
    uint32_t value;
    char tag[8];
} test_item_t;

/* Declare and define a queue type for testing */
DECLARE_LOCKFREE_QUEUE(test_item_t, test_queue)
DEFINE_LOCKFREE_QUEUE(test_item_t, test_queue)

/* Test fixture */
static test_queue_t queue;

static void setUp(void) {
    test_queue_init(&queue);
}

static void tearDown(void) {
    /* Drain any remaining items before destroy */
    /* This prevents assertion failure in destroy (queue must be empty) */
    test_item_t item;
    while (test_queue_dequeue(&queue, &item)) {
        /* Discard items */
    }
    test_queue_destroy(&queue);
}

/* ----------------------------------------------------------------------------
 * Initialization Tests
 * ---------------------------------------------------------------------------- */

/* Test: Queue initializes empty */
void test_Queue_InitializesEmpty(void) {
    setUp();

    TEST_ASSERT_TRUE(test_queue_empty(&queue));
    TEST_ASSERT_EQUAL(0, test_queue_size(&queue));
    TEST_ASSERT_TRUE(test_queue_verify_invariants(&queue));

    tearDown();
}

/* Test: Queue has correct capacity after init */
void test_Queue_CorrectCapacity(void) {
    setUp();

    /* Capacity is LOCKFREE_QUEUE_SIZE - 1 due to ring buffer full detection */
    TEST_ASSERT_EQUAL(LOCKFREE_QUEUE_SIZE - 1, test_queue_capacity(&queue));

    tearDown();
}

/* Test: Queue stats are zeroed on init */
void test_Queue_StatsZeroedOnInit(void) {
    setUp();

    size_t total_enq, total_deq, failed_enq, peak;
    test_queue_get_stats(&queue, &total_enq, &total_deq, &failed_enq, &peak);

    TEST_ASSERT_EQUAL(0, total_enq);
    TEST_ASSERT_EQUAL(0, total_deq);
    TEST_ASSERT_EQUAL(0, failed_enq);
    TEST_ASSERT_EQUAL(0, peak);

    tearDown();
}

/* ----------------------------------------------------------------------------
 * Basic Enqueue/Dequeue Tests
 * ---------------------------------------------------------------------------- */

/* Test: Single enqueue and dequeue */
void test_Queue_SingleEnqueueDequeue(void) {
    setUp();

    test_item_t item_in = {.id = 1, .value = 42, .tag = "TEST"};
    test_item_t item_out = {0};

    TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item_in));
    TEST_ASSERT_FALSE(test_queue_empty(&queue));
    TEST_ASSERT_EQUAL(1, test_queue_size(&queue));

    TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item_out));
    TEST_ASSERT_TRUE(test_queue_empty(&queue));
    TEST_ASSERT_EQUAL(0, test_queue_size(&queue));

    TEST_ASSERT_EQUAL(1, item_out.id);
    TEST_ASSERT_EQUAL(42, item_out.value);
    TEST_ASSERT_EQUAL_STRING("TEST", item_out.tag);

    tearDown();
}

/* Test: Multiple enqueue then dequeue */
void test_Queue_MultipleEnqueueDequeue(void) {
    setUp();

    const int count = 100;
    test_item_t item;

    /* Enqueue items */
    for (int i = 0; i < count; i++) {
        item.id = (uint32_t)i;
        item.value = (uint32_t)(i * 10);
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    }

    TEST_ASSERT_EQUAL(count, test_queue_size(&queue));

    /* Dequeue and verify */
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
        TEST_ASSERT_EQUAL((uint32_t)i, item.id);
        TEST_ASSERT_EQUAL((uint32_t)(i * 10), item.value);
    }

    TEST_ASSERT_TRUE(test_queue_empty(&queue));

    tearDown();
}

/* Test: Interleaved enqueue and dequeue */
void test_Queue_InterleavedOperations(void) {
    setUp();

    test_item_t item;

    /* Enqueue 3 */
    for (int i = 0; i < 3; i++) {
        item.id = (uint32_t)i;
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    }
    TEST_ASSERT_EQUAL(3, test_queue_size(&queue));

    /* Dequeue 2 */
    TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
    TEST_ASSERT_EQUAL(0, item.id);
    TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
    TEST_ASSERT_EQUAL(1, item.id);
    TEST_ASSERT_EQUAL(1, test_queue_size(&queue));

    /* Enqueue 2 more */
    item.id = 10;
    TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    item.id = 11;
    TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    TEST_ASSERT_EQUAL(3, test_queue_size(&queue));

    /* Dequeue all */
    TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
    TEST_ASSERT_EQUAL(2, item.id);  /* Original item 2 */
    TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
    TEST_ASSERT_EQUAL(10, item.id);
    TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
    TEST_ASSERT_EQUAL(11, item.id);

    TEST_ASSERT_TRUE(test_queue_empty(&queue));

    tearDown();
}

/* ----------------------------------------------------------------------------
 * FIFO Order Tests
 * ---------------------------------------------------------------------------- */

/* Test: FIFO ordering is preserved */
void test_Queue_FIFOOrder(void) {
    setUp();

    test_item_t item;
    const int count = 1000;

    /* Enqueue items with sequential IDs */
    for (int i = 0; i < count; i++) {
        item.id = (uint32_t)i;
        item.value = (uint32_t)(i * 7);  /* Arbitrary transformation */
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    }

    /* Dequeue and verify strict FIFO order */
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
        TEST_ASSERT_EQUAL((uint32_t)i, item.id);
        TEST_ASSERT_EQUAL((uint32_t)(i * 7), item.value);
    }

    tearDown();
}

/* ----------------------------------------------------------------------------
 * Boundary Condition Tests
 * ---------------------------------------------------------------------------- */

/* Test: Dequeue from empty queue fails */
void test_Queue_DequeueFromEmptyFails(void) {
    setUp();

    test_item_t item;
    TEST_ASSERT_FALSE(test_queue_dequeue(&queue, &item));

    /* Note: Empty queue dequeue is NOT tracked as a failure in the new API.
     * This is intentional - polling an empty queue is normal SPSC operation,
     * not an error condition. */

    tearDown();
}

/* Test: Enqueue to full queue fails */
void test_Queue_EnqueueToFullFails(void) {
    setUp();

    test_item_t item = {.id = 0, .value = 0};

    /* Fill the queue (capacity - 1 due to ring buffer implementation) */
    size_t max_items = LOCKFREE_QUEUE_SIZE - 1;
    for (size_t i = 0; i < max_items; i++) {
        item.id = (uint32_t)i;
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    }

    /* Next enqueue should fail */
    item.id = 9999;
    TEST_ASSERT_FALSE(test_queue_enqueue(&queue, &item));

    /* Verify failed enqueue is tracked */
    size_t failed_enq;
    test_queue_get_stats(&queue, NULL, NULL, &failed_enq, NULL);
    TEST_ASSERT_EQUAL(1, failed_enq);

    /* Queue should still function after failed enqueue */
    TEST_ASSERT_TRUE(test_queue_verify_invariants(&queue));

    /* Dequeue one and try again */
    TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
    TEST_ASSERT_EQUAL(0, item.id);  /* First item enqueued */

    /* Now enqueue should succeed */
    item.id = 9999;
    TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));

    /* tearDown will drain remaining items */
    tearDown();
}

/* Test: Ring buffer wrap-around */
void test_Queue_WrapAround(void) {
    setUp();

    test_item_t item;

    /* Fill half, drain half, repeat multiple times to force wrap-around */
    for (int cycle = 0; cycle < 5; cycle++) {
        /* Add batch */
        for (int i = 0; i < 1000; i++) {
            item.id = (uint32_t)(cycle * 1000 + i);
            TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
        }

        /* Remove batch and verify order */
        for (int i = 0; i < 1000; i++) {
            TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
            TEST_ASSERT_EQUAL((uint32_t)(cycle * 1000 + i), item.id);
        }
    }

    TEST_ASSERT_TRUE(test_queue_empty(&queue));
    TEST_ASSERT_TRUE(test_queue_verify_invariants(&queue));

    tearDown();
}

/* ----------------------------------------------------------------------------
 * Batch Dequeue Tests
 * ---------------------------------------------------------------------------- */

/* Test: Batch dequeue returns correct count */
void test_Queue_BatchDequeueCount(void) {
    setUp();

    test_item_t item = {.id = 0};
    test_item_t batch[10];

    /* Enqueue 5 items */
    for (int i = 0; i < 5; i++) {
        item.id = (uint32_t)i;
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    }

    /* Request 10, should get 5 */
    size_t count = test_queue_dequeue_batch(&queue, batch, 10);
    TEST_ASSERT_EQUAL(5, count);
    TEST_ASSERT_TRUE(test_queue_empty(&queue));

    tearDown();
}

/* Test: Batch dequeue preserves FIFO order */
void test_Queue_BatchDequeueFIFO(void) {
    setUp();

    test_item_t item;
    test_item_t batch[100];

    /* Enqueue 100 items */
    for (int i = 0; i < 100; i++) {
        item.id = (uint32_t)i;
        item.value = (uint32_t)(i * 3);
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    }

    /* Batch dequeue all */
    size_t count = test_queue_dequeue_batch(&queue, batch, 100);
    TEST_ASSERT_EQUAL(100, count);

    /* Verify FIFO order */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL((uint32_t)i, batch[i].id);
        TEST_ASSERT_EQUAL((uint32_t)(i * 3), batch[i].value);
    }

    tearDown();
}

/* Test: Multiple batch dequeues */
void test_Queue_MultipleBatchDequeues(void) {
    setUp();

    test_item_t item;
    test_item_t batch[32];

    /* Enqueue 100 items */
    for (int i = 0; i < 100; i++) {
        item.id = (uint32_t)i;
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    }

    /* Dequeue in batches of 32 */
    size_t total = 0;
    size_t count;

    count = test_queue_dequeue_batch(&queue, batch, 32);
    TEST_ASSERT_EQUAL(32, count);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL((uint32_t)(total + i), batch[i].id);
    }
    total += count;

    count = test_queue_dequeue_batch(&queue, batch, 32);
    TEST_ASSERT_EQUAL(32, count);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL((uint32_t)(total + i), batch[i].id);
    }
    total += count;

    count = test_queue_dequeue_batch(&queue, batch, 32);
    TEST_ASSERT_EQUAL(32, count);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL((uint32_t)(total + i), batch[i].id);
    }
    total += count;

    /* Last batch should have 4 items (100 - 96) */
    count = test_queue_dequeue_batch(&queue, batch, 32);
    TEST_ASSERT_EQUAL(4, count);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL((uint32_t)(total + i), batch[i].id);
    }

    TEST_ASSERT_TRUE(test_queue_empty(&queue));

    tearDown();
}

/* Test: Batch dequeue from empty queue */
void test_Queue_BatchDequeueEmpty(void) {
    setUp();

    test_item_t batch[10];

    size_t count = test_queue_dequeue_batch(&queue, batch, 10);
    TEST_ASSERT_EQUAL(0, count);

    tearDown();
}

/* Test: Batch dequeue with max_items = 0 */
void test_Queue_BatchDequeueZeroMax(void) {
    setUp();

    test_item_t item = {.id = 1};
    test_item_t batch[10];

    test_queue_enqueue(&queue, &item);

    size_t count = test_queue_dequeue_batch(&queue, batch, 0);
    TEST_ASSERT_EQUAL(0, count);
    TEST_ASSERT_EQUAL(1, test_queue_size(&queue));  /* Item still there */

    tearDown();
}

/* ----------------------------------------------------------------------------
 * Statistics Tests
 * ---------------------------------------------------------------------------- */

/* Test: Enqueue stats are tracked */
void test_Queue_EnqueueStatsTracked(void) {
    setUp();

    test_item_t item = {.id = 0};
    const int count = 50;

    for (int i = 0; i < count; i++) {
        test_queue_enqueue(&queue, &item);
    }

    size_t total_enq;
    test_queue_get_stats(&queue, &total_enq, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(count, total_enq);

    /* tearDown will drain remaining items */
    tearDown();
}

/* Test: Dequeue stats are tracked */
void test_Queue_DequeueStatsTracked(void) {
    setUp();

    test_item_t item = {.id = 0};
    const int enqueue_count = 50;
    const int dequeue_count = 30;

    for (int i = 0; i < enqueue_count; i++) {
        test_queue_enqueue(&queue, &item);
    }

    for (int i = 0; i < dequeue_count; i++) {
        test_queue_dequeue(&queue, &item);
    }

    size_t total_deq;
    test_queue_get_stats(&queue, NULL, &total_deq, NULL, NULL);
    TEST_ASSERT_EQUAL(dequeue_count, total_deq);

    /* tearDown will drain remaining items */
    tearDown();
}

/* Test: Peak size is tracked */
void test_Queue_PeakSizeTracked(void) {
    setUp();

    test_item_t item = {.id = 0};

    /* Add 100 items */
    for (int i = 0; i < 100; i++) {
        test_queue_enqueue(&queue, &item);
    }

    /* Remove 50 */
    for (int i = 0; i < 50; i++) {
        test_queue_dequeue(&queue, &item);
    }

    /* Add 30 more (current size = 80, peak should still be 100) */
    for (int i = 0; i < 30; i++) {
        test_queue_enqueue(&queue, &item);
    }

    size_t peak;
    test_queue_get_stats(&queue, NULL, NULL, NULL, &peak);
    TEST_ASSERT_EQUAL(100, peak);

    /* tearDown will drain remaining items */
    tearDown();
}

/* Test: Batch dequeue stats are tracked */
void test_Queue_BatchDequeueStatsTracked(void) {
    setUp();

    test_item_t item = {.id = 0};
    test_item_t batch[32];
    const int enqueue_count = 100;

    for (int i = 0; i < enqueue_count; i++) {
        test_queue_enqueue(&queue, &item);
    }

    /* Dequeue in batches */
    test_queue_dequeue_batch(&queue, batch, 32);
    test_queue_dequeue_batch(&queue, batch, 32);
    test_queue_dequeue_batch(&queue, batch, 32);
    test_queue_dequeue_batch(&queue, batch, 32);  /* Gets remaining 4 */

    size_t total_deq;
    test_queue_get_stats(&queue, NULL, &total_deq, NULL, NULL);
    TEST_ASSERT_EQUAL(enqueue_count, total_deq);

    tearDown();
}

/* ----------------------------------------------------------------------------
 * Invariant Tests
 * ---------------------------------------------------------------------------- */

/* Test: Invariants hold after operations */
void test_Queue_InvariantsHold(void) {
    setUp();

    test_item_t item = {.id = 0};

    /* After init */
    TEST_ASSERT_TRUE(test_queue_verify_invariants(&queue));

    /* After some enqueues */
    for (int i = 0; i < 50; i++) {
        test_queue_enqueue(&queue, &item);
    }
    TEST_ASSERT_TRUE(test_queue_verify_invariants(&queue));

    /* After some dequeues */
    for (int i = 0; i < 25; i++) {
        test_queue_dequeue(&queue, &item);
    }
    TEST_ASSERT_TRUE(test_queue_verify_invariants(&queue));

    /* After draining */
    while (!test_queue_empty(&queue)) {
        test_queue_dequeue(&queue, &item);
    }
    TEST_ASSERT_TRUE(test_queue_verify_invariants(&queue));

    tearDown();
}

/* ----------------------------------------------------------------------------
 * NULL Safety Tests
 * ---------------------------------------------------------------------------- */

/* Test: NULL queue handling - these should trigger assertions in debug mode */
/* In release mode, they return safe defaults */
void test_Queue_NullQueueHandling(void) {
    /* Note: In debug builds, these would assert. In release, they return safe values. */
    /* We test that they don't crash at minimum. */
#ifndef NDEBUG
    /* Skip NULL tests in debug mode - they would assert */
    return;
#else
    test_item_t item = {.id = 0};
    test_item_t batch[10];

    TEST_ASSERT_FALSE(test_queue_enqueue(NULL, &item));
    TEST_ASSERT_FALSE(test_queue_dequeue(NULL, &item));
    TEST_ASSERT_EQUAL(0, test_queue_dequeue_batch(NULL, batch, 10));
    TEST_ASSERT_TRUE(test_queue_empty(NULL));
    TEST_ASSERT_EQUAL(0, test_queue_size(NULL));
    TEST_ASSERT_FALSE(test_queue_verify_invariants(NULL));
#endif
}

/* Test: NULL item handling */
void test_Queue_NullItemHandling(void) {
#ifndef NDEBUG
    /* Skip NULL tests in debug mode - they would assert */
    return;
#else
    setUp();

    TEST_ASSERT_FALSE(test_queue_enqueue(&queue, NULL));
    TEST_ASSERT_FALSE(test_queue_dequeue(&queue, NULL));
    TEST_ASSERT_EQUAL(0, test_queue_dequeue_batch(&queue, NULL, 10));

    tearDown();
#endif
}

/* ----------------------------------------------------------------------------
 * Data Integrity Tests
 * ---------------------------------------------------------------------------- */

/* Test: All struct fields are preserved */
void test_Queue_DataIntegrity(void) {
    setUp();

    test_item_t items_in[10];
    test_item_t items_out[10];

    /* Create items with distinct data */
    for (int i = 0; i < 10; i++) {
        items_in[i].id = (uint32_t)(i + 100);
        items_in[i].value = (uint32_t)(i * 1000 + 500);
        snprintf(items_in[i].tag, sizeof(items_in[i].tag), "T%d", i);
    }

    /* Enqueue all */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &items_in[i]));
    }

    /* Dequeue all */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &items_out[i]));
    }

    /* Verify all fields match */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(items_in[i].id, items_out[i].id);
        TEST_ASSERT_EQUAL(items_in[i].value, items_out[i].value);
        TEST_ASSERT_EQUAL_STRING(items_in[i].tag, items_out[i].tag);
    }

    tearDown();
}

/* Test: Data integrity with batch dequeue */
void test_Queue_DataIntegrityBatch(void) {
    setUp();

    test_item_t items_in[50];
    test_item_t items_out[50];

    /* Create items with distinct data */
    for (int i = 0; i < 50; i++) {
        items_in[i].id = (uint32_t)(i + 200);
        items_in[i].value = (uint32_t)(i * 7 + 13);
        snprintf(items_in[i].tag, sizeof(items_in[i].tag), "B%02d", i);
    }

    /* Enqueue all */
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &items_in[i]));
    }

    /* Batch dequeue all */
    size_t count = test_queue_dequeue_batch(&queue, items_out, 50);
    TEST_ASSERT_EQUAL(50, count);

    /* Verify all fields match */
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL(items_in[i].id, items_out[i].id);
        TEST_ASSERT_EQUAL(items_in[i].value, items_out[i].value);
        TEST_ASSERT_EQUAL_STRING(items_in[i].tag, items_out[i].tag);
    }

    tearDown();
}

/* ----------------------------------------------------------------------------
 * Size Consistency Tests
 * ---------------------------------------------------------------------------- */

/* Test: Size is consistent with operations */
void test_Queue_SizeConsistency(void) {
    setUp();

    test_item_t item = {.id = 0};

    TEST_ASSERT_EQUAL(0, test_queue_size(&queue));

    test_queue_enqueue(&queue, &item);
    TEST_ASSERT_EQUAL(1, test_queue_size(&queue));

    test_queue_enqueue(&queue, &item);
    TEST_ASSERT_EQUAL(2, test_queue_size(&queue));

    test_queue_dequeue(&queue, &item);
    TEST_ASSERT_EQUAL(1, test_queue_size(&queue));

    test_queue_dequeue(&queue, &item);
    TEST_ASSERT_EQUAL(0, test_queue_size(&queue));

    tearDown();
}

/* Test: Size is consistent with batch dequeue */
void test_Queue_SizeConsistencyBatch(void) {
    setUp();

    test_item_t item = {.id = 0};
    test_item_t batch[32];

    /* Add 100 items */
    for (int i = 0; i < 100; i++) {
        test_queue_enqueue(&queue, &item);
    }
    TEST_ASSERT_EQUAL(100, test_queue_size(&queue));

    /* Batch dequeue 32 */
    test_queue_dequeue_batch(&queue, batch, 32);
    TEST_ASSERT_EQUAL(68, test_queue_size(&queue));

    /* Batch dequeue 32 more */
    test_queue_dequeue_batch(&queue, batch, 32);
    TEST_ASSERT_EQUAL(36, test_queue_size(&queue));

    tearDown();
}

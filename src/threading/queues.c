/**
 * queues.c - Queue type instantiation
 *
 * This file instantiates the lock-free queue types using the macro-based
 * template system defined in lockfree_queue.h.
 *
 * Queue Types:
 *   input_envelope_queue_t  - Input messages from network to processor
 *   output_envelope_queue_t - Output messages from processor to router
 *
 * Power of Ten Compliance:
 *   - Rule 3: No dynamic allocation (fixed-size ring buffers)
 *   - All other rules enforced in lockfree_queue.h macros
 *
 * Performance Notes:
 *   - Both queues are SPSC (single-producer, single-consumer)
 *   - Producer and consumer stats on separate cache lines
 *   - Batch dequeue available for reduced atomic operations
 */

#include "threading/queues.h"

/* ============================================================================
 * Queue Instantiations
 * ============================================================================
 *
 * DEFINE_LOCKFREE_QUEUE generates the following functions for each queue:
 *
 *   void {name}_init({name}_t* queue)
 *   void {name}_destroy({name}_t* queue)
 *   bool {name}_enqueue({name}_t* queue, const TYPE* item)
 *   bool {name}_dequeue({name}_t* queue, TYPE* item)
 *   size_t {name}_dequeue_batch({name}_t* queue, TYPE* items, size_t max)
 *   bool {name}_empty(const {name}_t* queue)
 *   size_t {name}_size(const {name}_t* queue)
 *   size_t {name}_capacity(const {name}_t* queue)
 *   bool {name}_verify_invariants(const {name}_t* queue)
 *   void {name}_get_stats(const {name}_t* queue, ...)
 */

/* Input envelope queue: Network receiver → Processor */
DEFINE_LOCKFREE_QUEUE(input_msg_envelope_t, input_envelope_queue)

/* Output envelope queue: Processor → Output router */
DEFINE_LOCKFREE_QUEUE(output_msg_envelope_t, output_envelope_queue)

/*
 * Future queue types can be added here as needed:
 *
 * Example:
 *   DEFINE_LOCKFREE_QUEUE(market_data_t, market_data_queue)
 *   DEFINE_LOCKFREE_QUEUE(risk_update_t, risk_queue)
 */

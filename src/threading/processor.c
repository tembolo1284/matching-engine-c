#include "threading/processor.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

static const struct timespec sleep_ts = {
    .tv_sec = 0,
    .tv_nsec = PROCESSOR_SLEEP_NS
};

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool processor_init(processor_t* processor,
                    const processor_config_t* config,
                    matching_engine_t* engine,
                    input_envelope_queue_t* input_queue,
                    output_envelope_queue_t* output_queue,
                    atomic_bool* shutdown_flag) {
    /* Rule 5: Validate parameters */
    assert(processor != NULL);
    assert(config != NULL);
    assert(engine != NULL);
    assert(input_queue != NULL);
    assert(output_queue != NULL);
    assert(shutdown_flag != NULL);

    memset(processor, 0, sizeof(*processor));

    processor->config = *config;
    processor->engine = engine;
    processor->input_queue = input_queue;
    processor->output_queue = output_queue;
    processor->shutdown_flag = shutdown_flag;

    atomic_init(&processor->running, false);
    atomic_init(&processor->started, false);
    atomic_init(&processor->output_sequence, 0);

    /* Initialize statistics - regular uint64_t, not atomic */
    processor->stats.messages_processed = 0;
    processor->stats.batches_processed = 0;
    processor->stats.output_messages = 0;
    processor->stats.trades_processed = 0;
    processor->stats.empty_polls = 0;
    processor->stats.output_queue_full = 0;

    return true;
}

void processor_cleanup(processor_t* processor) {
    assert(processor != NULL);
    /* Nothing to clean up - no dynamic allocation (Rule 3) */
    (void)processor;
}

/* ============================================================================
 * Helper: Enqueue output with error tracking
 * ============================================================================ */

static inline bool enqueue_output(processor_t* processor,
                                  const output_msg_t* msg,
                                  uint32_t client_id,
                                  uint64_t seq,
                                  uint64_t* local_output_count,
                                  uint64_t* local_queue_full) {
    output_msg_envelope_t env = create_output_envelope(msg, client_id, seq);

    if (output_envelope_queue_enqueue(processor->output_queue, &env)) {
        (*local_output_count)++;
        return true;
    } else {
        (*local_queue_full)++;
        return false;
    }
}

/* ============================================================================
 * Helper: Drain output buffer to queue
 * ============================================================================ */

static inline void drain_output_buffer(processor_t* processor,
                                       output_buffer_t* output_buffer,
                                       uint32_t client_id,
                                       uint64_t* local_sequence,
                                       uint64_t* local_outputs,
                                       uint64_t* local_trades,
                                       uint64_t* local_queue_full) {
    /* Rule 2: Loop bounded by output_buffer->count <= MAX_OUTPUT_MESSAGES */
    for (int j = 0; j < output_buffer->count; j++) {
        output_msg_t* out_msg = &output_buffer->messages[j];
        uint64_t seq = (*local_sequence)++;

        if (out_msg->type == OUTPUT_MSG_TRADE) {
            /* Trade: route to BOTH buyer and seller */
            (*local_trades)++;

            uint32_t buy_client = out_msg->data.trade.buy_client_id;
            uint32_t sell_client = out_msg->data.trade.sell_client_id;

            /* Send to buyer */
            enqueue_output(processor, out_msg, buy_client, seq,
                           local_outputs, local_queue_full);

            /* Send to seller (if different client) */
            if (buy_client != sell_client) {
                enqueue_output(processor, out_msg, sell_client, seq,
                               local_outputs, local_queue_full);
            }
        } else {
            /* Ack, CancelAck, TopOfBook: route to originating client */
            enqueue_output(processor, out_msg, client_id, seq,
                           local_outputs, local_queue_full);
        }
    }

    /* Reset buffer for next use */
    output_buffer->count = 0;
}

/* ============================================================================
 * Main Processing Loop
 * ============================================================================ */

void* processor_thread(void* arg) {
    processor_t* processor = (processor_t*)arg;
    assert(processor != NULL);

    fprintf(stderr, "[Processor %d] Starting (wait:sleep)\n", ctx->processor_id);

    atomic_store(&processor->started, true);
    atomic_store(&processor->running, true);

    /* Pre-allocated batch buffer - no allocation in loop (Rule 3) */
    input_msg_envelope_t input_batch[PROCESSOR_BATCH_SIZE];

    /* Output buffer - reused across all messages in batch */
    output_buffer_t output_buffer;

    /* Local counters - batched update to atomics */
    uint64_t local_messages = 0;
    uint64_t local_batches = 0;
    uint64_t local_outputs = 0;
    uint64_t local_trades = 0;
    uint64_t local_empty_polls = 0;
    uint64_t local_queue_full = 0;

    /* Local sequence counter - reduces atomic contention */
    uint64_t local_sequence = atomic_load(&processor->output_sequence);

    /* Spin counter for hybrid wait strategy */
    int spin_count = 0;

    /* Stats flush interval */
    const uint64_t STATS_FLUSH_INTERVAL = 1000;
    uint64_t since_last_flush = 0;

    /* Track client_id for flush continuation (0 = UDP/broadcast) */
    uint32_t flush_client_id = 0;

    /* Main loop - Rule 2: bounded by shutdown flag */
    while (!atomic_load(processor->shutdown_flag)) {

        /* ================================================================
         * Phase 0: Continue any in-progress flush operations
         * ================================================================ */
        if (matching_engine_has_flush_in_progress(processor->engine)) {
            output_buffer.count = 0;

            /* Continue flush - processes one batch of cancels */
            bool flush_done = matching_engine_continue_flush(processor->engine, &output_buffer);

            /* Drain output buffer */
            if (output_buffer.count > 0) {
                drain_output_buffer(processor, &output_buffer, flush_client_id,
                                    &local_sequence, &local_outputs,
                                    &local_trades, &local_queue_full);
            }

            /* If flush not done, skip to next iteration to continue draining */
            if (!flush_done) {
                continue;
            }
        }

        /* ================================================================
         * Phase 1: Dequeue batch of input messages
         * ================================================================ */
        size_t count = 0;

        /* Rule 2: Loop bounded by PROCESSOR_BATCH_SIZE */
        for (size_t i = 0; i < PROCESSOR_BATCH_SIZE; i++) {
            if (input_envelope_queue_dequeue(processor->input_queue, &input_batch[count])) {
                count++;
            } else {
                /* Queue empty - could retry, but usually means we got everything */
                break;
            }
        }

        /* ================================================================
         * Phase 2: Handle empty queue (spin or sleep)
         * ================================================================ */
        if (count == 0) {
            local_empty_polls++;

            if (processor->config.spin_wait) {
                /* Spin-wait: better latency, uses more CPU */
                spin_count++;
                if (spin_count >= PROCESSOR_SPIN_ITERATIONS) {
                    /* Yield after spinning to avoid starving other threads */
                    sched_yield();
                    spin_count = 0;
                }
                /* CPU pause hint - reduces power, helps hyperthreading */
                #if defined(__x86_64__)
                __asm__ volatile("pause" ::: "memory");
                #elif defined(__aarch64__)
                __asm__ volatile("yield" ::: "memory");
                #endif
            } else {
                /* Sleep-wait: saves CPU, adds latency */
                nanosleep(&sleep_ts, NULL);
            }
            continue;
        }

        /* Reset spin counter on successful dequeue */
        spin_count = 0;
        local_batches++;

        /* ================================================================
         * Phase 3: Process each message in batch
         * ================================================================ */

        /* Rule 2: Loop bounded by count <= PROCESSOR_BATCH_SIZE */
        for (size_t i = 0; i < count; i++) {
            input_msg_envelope_t* envelope = &input_batch[i];

            /* Prefetch next message while processing current */
            if (i + 1 < count) {
                PREFETCH_READ(&input_batch[i + 1]);
            }

            input_msg_t* msg = &envelope->msg;
            uint32_t client_id = envelope->client_id;

            /* Reset output buffer (just set count to 0, no memset) */
            output_buffer.count = 0;

            /* Track client_id for flush continuation */
            if (msg->type == INPUT_MSG_FLUSH) {
                flush_client_id = client_id;
            }

            /* Process through matching engine */
            matching_engine_process_message(processor->engine, msg, client_id, &output_buffer);

            /* ============================================================
             * Phase 4: Route outputs to appropriate clients
             * ============================================================ */
            drain_output_buffer(processor, &output_buffer, client_id,
                                &local_sequence, &local_outputs,
                                &local_trades, &local_queue_full);

            local_messages++;
        }

        /* ================================================================
         * Phase 5: Periodic flush of statistics to atomics
         * ================================================================ */
        since_last_flush += count;

        if (since_last_flush >= STATS_FLUSH_INTERVAL) {
            /* Batch update stats - single write per counter */
            processor->stats.messages_processed += local_messages;
            processor->stats.batches_processed += local_batches;
            processor->stats.output_messages += local_outputs;
            processor->stats.trades_processed += local_trades;
            processor->stats.empty_polls += local_empty_polls;
            processor->stats.output_queue_full += local_queue_full;
            atomic_store(&processor->output_sequence, local_sequence);

            /* Reset local counters */
            local_messages = 0;
            local_batches = 0;
            local_outputs = 0;
            local_trades = 0;
            local_empty_polls = 0;
            local_queue_full = 0;
            since_last_flush = 0;
        }
    }

    /* Final flush of remaining statistics */
    processor->stats.messages_processed += local_messages;
    processor->stats.batches_processed += local_batches;
    processor->stats.output_messages += local_outputs;
    processor->stats.trades_processed += local_trades;
    processor->stats.empty_polls += local_empty_polls;
    processor->stats.output_queue_full += local_queue_full;
    atomic_store(&processor->output_sequence, local_sequence);

    atomic_store(&processor->running, false);

    fprintf(stderr, "[Processor %d] Shutting down\n", processor->config.processor_id);
    processor_print_stats(processor);

    return NULL;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void processor_print_stats(const processor_t* processor) {
    assert(processor != NULL);

    /* Read stats - may be slightly stale, acceptable for monitoring */
    uint64_t messages = processor->stats.messages_processed;
    uint64_t batches = processor->stats.batches_processed;
    uint64_t outputs = processor->stats.output_messages;
    uint64_t trades = processor->stats.trades_processed;
    uint64_t empty = processor->stats.empty_polls;
    uint64_t full = processor->stats.output_queue_full;

    double avg_batch = batches > 0 ? (double)messages / batches : 0.0;
    double outputs_per_msg = messages > 0 ? (double)outputs / messages : 0.0;

    fprintf(stderr, "\n=== Processor %d Statistics ===\n",
            processor->config.processor_id);
    fprintf(stderr, "Messages processed:    %llu\n", (unsigned long long)messages);
    fprintf(stderr, "Batches processed:     %llu\n", (unsigned long long)batches);
    fprintf(stderr, "Average batch size:    %.1f\n", avg_batch);
    fprintf(stderr, "Output messages:       %llu\n", (unsigned long long)outputs);
    fprintf(stderr, "Outputs per message:   %.2f\n", outputs_per_msg);
    fprintf(stderr, "Trades processed:      %llu\n", (unsigned long long)trades);
    fprintf(stderr, "Empty polls:           %llu\n", (unsigned long long)empty);
    fprintf(stderr, "Output queue full:     %llu\n", (unsigned long long)full);
}

/* ============================================================================
 * Client Order Cancellation
 * ============================================================================ */

void processor_cancel_client_orders(processor_t* processor, uint32_t client_id) {
    assert(processor != NULL);
    assert(client_id > 0);  /* 0 is reserved for broadcast/UDP */

    fprintf(stderr, "[Processor %d] Cancelling all orders for client %u\n",
            processor->config.processor_id, client_id);

    /* Output buffer for cancel acknowledgements */
    output_buffer_t output_buffer;
    output_buffer.count = 0;

    /* Call matching engine to cancel all orders for this client */
    size_t cancelled = matching_engine_cancel_client_orders(
        processor->engine,
        client_id,
        &output_buffer
    );

    fprintf(stderr, "[Processor %d] Cancelled %zu orders for client %u\n",
            processor->config.processor_id, cancelled, client_id);

    /* Enqueue cancel acknowledgements */
    uint64_t local_seq = atomic_fetch_add(&processor->output_sequence,
                                          (uint64_t)output_buffer.count);

    /* Rule 2: Loop bounded by output_buffer.count */
    for (int i = 0; i < output_buffer.count; i++) {
        output_msg_t* out_msg = &output_buffer.messages[i];

        output_msg_envelope_t envelope = create_output_envelope(
            out_msg, client_id, local_seq + i);

        if (!output_envelope_queue_enqueue(processor->output_queue, &envelope)) {
            fprintf(stderr, "[Processor %d] Output queue full during client cancel!\n",
                    processor->config.processor_id);
            processor->stats.output_queue_full++;
        } else {
            processor->stats.output_messages++;
        }
    }
}

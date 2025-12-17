#include "threading/output_publisher.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_formatter.h"
#include "threading/queues.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define BATCH_SIZE 32
#define PROGRESS_INTERVAL_NS (30ULL * 1000000000ULL)  /* 30 seconds */

static const struct timespec sleep_ts = {
    .tv_sec = 0,
    .tv_nsec = 1000  /* 1 microsecond */
};

/* Global formatter instances - Rule 3: no dynamic allocation */
static message_formatter_t g_csv_formatter;
static binary_message_formatter_t g_binary_formatter;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Get current time in nanoseconds
 * Rule 7: Check return value of clock_gettime
 */
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0 && "clock_gettime failed");
    (void)rc;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * Update message type statistics
 */
static inline void update_type_stats(output_publisher_context_t* ctx,
                                     output_msg_type_t type) {
    assert(ctx != NULL && "NULL ctx in update_type_stats");

    switch (type) {
        case OUTPUT_MSG_ACK:
            ctx->acks_published++;
            break;
        case OUTPUT_MSG_TRADE:
            ctx->trades_published++;
            break;
        case OUTPUT_MSG_CANCEL_ACK:
            ctx->cancels_published++;
            break;
        case OUTPUT_MSG_TOP_OF_BOOK:
            ctx->tob_updates_published++;
            break;
        default:
            /* Unknown type - just count it */
            break;
    }
}

/**
 * Format and write a single message to stdout
 * Returns number of bytes written, 0 on error
 */
static size_t format_and_write(output_publisher_context_t* ctx,
                               const output_msg_t* msg,
                               char* buffer,
                               size_t buffer_size) {
    assert(ctx != NULL && "NULL ctx in format_and_write");
    assert(msg != NULL && "NULL msg in format_and_write");
    assert(buffer != NULL && "NULL buffer in format_and_write");
    assert(buffer_size > 0 && "Zero buffer_size");

    size_t len = 0;

    if (ctx->config.use_binary_output) {
        /* Binary formatting */
        size_t bin_len;
        const void* bin_data = binary_message_formatter_format(
            &g_binary_formatter, msg, &bin_len
        );

        if (bin_data && bin_len <= buffer_size) {
            memcpy(buffer, bin_data, bin_len);
            len = bin_len;
        }
    } else {
        /* CSV formatting */
        const char* csv_str = message_formatter_format(&g_csv_formatter, msg);

        if (csv_str) {
            len = strlen(csv_str);
            if (len + 1 < buffer_size) {  /* +1 for newline */
                memcpy(buffer, csv_str, len);
                buffer[len] = '\n';
                len++;
            } else {
                len = 0;  /* Too long, skip */
            }
        }
    }

    if (len > 0) {
        size_t written = fwrite(buffer, 1, len, stdout);
        if (written != len) {
            /* Rule 7: Check fwrite return value */
            fprintf(stderr, "[Output Publisher] WARNING: fwrite incomplete: %zu/%zu\n",
                    written, len);
        }
        /* Flush after each message for real-time output */
        int rc = fflush(stdout);
        if (rc != 0) {
            fprintf(stderr, "[Output Publisher] WARNING: fflush failed\n");
        }
        return written;
    }

    return 0;
}

/**
 * Print progress update
 */
static void print_progress(output_publisher_context_t* ctx,
                          uint64_t start_time_ns,
                          uint64_t now_ns,
                          uint64_t* last_progress_time,
                          uint64_t* last_progress_msgs) {
    assert(ctx != NULL && "NULL ctx in print_progress");
    assert(last_progress_time != NULL && "NULL last_progress_time");
    assert(last_progress_msgs != NULL && "NULL last_progress_msgs");

    uint64_t elapsed_ns = now_ns - start_time_ns;
    double elapsed_sec = (double)elapsed_ns / 1e9;
    uint64_t msgs_since_last = ctx->messages_published - *last_progress_msgs;
    double interval_sec = (double)(now_ns - *last_progress_time) / 1e9;
    double current_rate = (interval_sec > 0) ? (msgs_since_last / interval_sec) : 0;
    double avg_rate = (elapsed_sec > 0) ? (ctx->messages_published / elapsed_sec) : 0;

    fprintf(stderr, "[PROGRESS] %6.1fs | %12llu msgs | %10llu trades | %8.2fK msg/s (avg: %.2fK)\n",
            elapsed_sec,
            (unsigned long long)ctx->messages_published,
            (unsigned long long)ctx->trades_published,
            current_rate / 1000.0,
            avg_rate / 1000.0);

    *last_progress_time = now_ns;
    *last_progress_msgs = ctx->messages_published;
}

/* ============================================================================
 * Initialization / Cleanup
 * ============================================================================ */

bool output_publisher_init(output_publisher_context_t* ctx,
                           const output_publisher_config_t* config,
                           output_envelope_queue_t* input_queue,
                           atomic_bool* shutdown_flag) {
    /* Rule 5: Preconditions */
    assert(ctx != NULL && "NULL ctx in output_publisher_init");
    assert(config != NULL && "NULL config in output_publisher_init");
    assert(input_queue != NULL && "NULL input_queue in output_publisher_init");
    assert(shutdown_flag != NULL && "NULL shutdown_flag in output_publisher_init");

    memset(ctx, 0, sizeof(*ctx));

    ctx->config = *config;
    ctx->input_queue = input_queue;
    ctx->shutdown_flag = shutdown_flag;

    /* Initialize statistics */
    ctx->messages_published = 0;
    ctx->acks_published = 0;
    ctx->trades_published = 0;
    ctx->cancels_published = 0;
    ctx->tob_updates_published = 0;

    /* Initialize formatters */
    message_formatter_init(&g_csv_formatter);
    binary_message_formatter_init(&g_binary_formatter);

    /* Rule 5: Postcondition */
    assert(ctx->messages_published == 0 && "stats not zero after init");

    return true;
}

void output_publisher_cleanup(output_publisher_context_t* ctx) {
    /* Rule 5: Precondition */
    assert(ctx != NULL && "NULL ctx in output_publisher_cleanup");
    (void)ctx;
}

/* ============================================================================
 * Publisher Thread
 * ============================================================================ */

void* output_publisher_thread(void* arg) {
    output_publisher_context_t* ctx = (output_publisher_context_t*)arg;
    assert(ctx != NULL && "NULL ctx in output_publisher_thread");

    fprintf(stderr, "[Output Publisher] Starting (format: %s, quiet: %s)\n",
            ctx->config.use_binary_output ? "Binary" : "CSV",
            ctx->config.quiet_mode ? "yes" : "no");

    /* Pre-allocated buffers - Rule 3: no allocation in loop */
    output_msg_envelope_t batch[BATCH_SIZE];
    char output_buffer[4096];

    /* Progress tracking */
    uint64_t start_time_ns = get_time_ns();
    uint64_t last_progress_time = start_time_ns;
    uint64_t last_progress_msgs = 0;

    /* Main loop - Rule 2: bounded by shutdown flag */
    while (!atomic_load(ctx->shutdown_flag)) {

        /* ================================================================
         * Phase 1: Batch dequeue
         * PERFORMANCE: Single atomic operation for up to BATCH_SIZE messages
         * ================================================================ */
        size_t count = output_envelope_queue_dequeue_batch(
            ctx->input_queue,
            batch,
            BATCH_SIZE
        );

        /* ================================================================
         * Phase 2: Handle empty queue
         * ================================================================ */
        if (count == 0) {
            nanosleep(&sleep_ts, NULL);

            /* Check for progress update even when idle (quiet mode only) */
            if (ctx->config.quiet_mode) {
                uint64_t now_ns = get_time_ns();
                if (now_ns - last_progress_time >= PROGRESS_INTERVAL_NS) {
                    print_progress(ctx, start_time_ns, now_ns,
                                  &last_progress_time, &last_progress_msgs);
                }
            }
            continue;
        }

        /* ================================================================
         * Phase 3: Process batch
         * Rule 2: Loop bounded by count <= BATCH_SIZE
         * ================================================================ */
        for (size_t i = 0; i < count; i++) {
            output_msg_t* msg = &batch[i].msg;

            /* Track message type statistics */
            update_type_stats(ctx, msg->type);

            /* In quiet mode, just count - don't output */
            if (ctx->config.quiet_mode) {
                ctx->messages_published++;
                continue;
            }

            /* Format and write to stdout */
            if (format_and_write(ctx, msg, output_buffer, sizeof(output_buffer)) > 0) {
                ctx->messages_published++;
            }
        }

        /* ================================================================
         * Phase 4: Periodic progress update (quiet mode)
         * ================================================================ */
        if (ctx->config.quiet_mode) {
            uint64_t now_ns = get_time_ns();
            if (now_ns - last_progress_time >= PROGRESS_INTERVAL_NS) {
                print_progress(ctx, start_time_ns, now_ns,
                              &last_progress_time, &last_progress_msgs);
            }
        }
    }

    /* ================================================================
     * Shutdown: Drain remaining messages
     * ================================================================ */
    fprintf(stderr, "[Output Publisher] Draining remaining messages...\n");

    int drain_iterations = 0;
    const int MAX_DRAIN_ITERATIONS = 100;

    /* Rule 2: Bounded drain loop */
    while (drain_iterations < MAX_DRAIN_ITERATIONS) {
        size_t count = output_envelope_queue_dequeue_batch(
            ctx->input_queue,
            batch,
            BATCH_SIZE
        );

        if (count == 0) {
            break;
        }

        drain_iterations++;

        for (size_t i = 0; i < count; i++) {
            output_msg_t* msg = &batch[i].msg;
            update_type_stats(ctx, msg->type);

            if (!ctx->config.quiet_mode) {
                if (format_and_write(ctx, msg, output_buffer, sizeof(output_buffer)) > 0) {
                    ctx->messages_published++;
                }
            } else {
                ctx->messages_published++;
            }
        }
    }

    if (drain_iterations >= MAX_DRAIN_ITERATIONS) {
        fprintf(stderr, "[Output Publisher] WARNING: Drain limit reached\n");
    }

    fprintf(stderr, "[Output Publisher] Shutting down\n");
    output_publisher_print_stats(ctx);

    return NULL;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void output_publisher_print_stats(const output_publisher_context_t* ctx) {
    /* Rule 5: Precondition */
    assert(ctx != NULL && "NULL ctx in output_publisher_print_stats");

    fprintf(stderr, "\n=== Output Publisher Statistics ===\n");
    fprintf(stderr, "Messages published:    %llu\n", (unsigned long long)ctx->messages_published);
    fprintf(stderr, "  Acks:                %llu\n", (unsigned long long)ctx->acks_published);
    fprintf(stderr, "  Trades:              %llu\n", (unsigned long long)ctx->trades_published);
    fprintf(stderr, "  Cancel Acks:         %llu\n", (unsigned long long)ctx->cancels_published);
    fprintf(stderr, "  TOB Updates:         %llu\n", (unsigned long long)ctx->tob_updates_published);

    /* Sanity check */
    uint64_t type_sum = ctx->acks_published + ctx->trades_published +
                        ctx->cancels_published + ctx->tob_updates_published;
    if (type_sum != ctx->messages_published) {
        fprintf(stderr, "  (Note: %llu messages with unknown/other types)\n",
                (unsigned long long)(ctx->messages_published - type_sum));
    }
}

#include "threading/output_publisher.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_formatter.h"
#include "threading/queues.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BATCH_SIZE 32
#define SLEEP_TIME_US 100

static const struct timespec ts = {
    .tv_sec = 0,
    .tv_nsec = 1000
};

// Global formatter instances
static message_formatter_t g_csv_formatter;
static binary_message_formatter_t g_binary_formatter;

bool output_publisher_init(output_publisher_context_t* ctx,
                           const output_publisher_config_t* config,
                           output_envelope_queue_t* input_queue,
                           atomic_bool* shutdown_flag) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->config = *config;
    ctx->input_queue = input_queue;
    ctx->shutdown_flag = shutdown_flag;

    // Initialize statistics
    ctx->messages_published = 0;
    ctx->acks_published = 0;
    ctx->trades_published = 0;
    ctx->cancels_published = 0;
    ctx->tob_updates_published = 0;

    // Initialize formatters
    message_formatter_init(&g_csv_formatter);
    binary_message_formatter_init(&g_binary_formatter);

    return true;
}

void output_publisher_cleanup(output_publisher_context_t* ctx) {
    (void)ctx;
}

void* output_publisher_thread(void* arg) {
    output_publisher_context_t* ctx = (output_publisher_context_t*)arg;

    fprintf(stderr, "[Output Publisher] Starting (format: %s, quiet: %s)\n",
            ctx->config.use_binary_output ? "Binary" : "CSV",
            ctx->config.quiet_mode ? "yes" : "no");

    output_msg_envelope_t batch[BATCH_SIZE];
    char output_buffer[4096];

    while (!atomic_load(ctx->shutdown_flag)) {
        // Dequeue batch of output envelopes
        size_t count = 0;
        for (size_t i = 0; i < BATCH_SIZE; i++) {
            if (output_envelope_queue_dequeue(ctx->input_queue, &batch[count])) {
                count++;
            } else {
                break;
            }
        }

        if (count == 0) {
            nanosleep(&ts, NULL);
            continue;
        }

        // Format and publish each message
        for (size_t i = 0; i < count; i++) {
            // Extract the actual message (ignore client_id in UDP mode)
            output_msg_t* msg = &batch[i].msg;

            // Track message type statistics
            switch (msg->type) {
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
                    break;
            }

            // Skip stdout output if in quiet mode
            if (ctx->config.quiet_mode) {
                ctx->messages_published++;
                continue;
            }

            size_t len = 0;

            if (ctx->config.use_binary_output) {
                // Binary formatting
                size_t bin_len;
                const void* bin_data = binary_message_formatter_format(
                    &g_binary_formatter, msg, &bin_len
                );

                if (bin_data && bin_len <= sizeof(output_buffer)) {
                    memcpy(output_buffer, bin_data, bin_len);
                    len = bin_len;
                }
            } else {
                // CSV formatting
                const char* csv_str = message_formatter_format(&g_csv_formatter, msg);

                if (csv_str) {
                    len = strlen(csv_str);
                    if (len < sizeof(output_buffer)) {
                        memcpy(output_buffer, csv_str, len);
                        // Add newline for CSV
                        if (len + 1 < sizeof(output_buffer)) {
                            output_buffer[len] = '\n';
                            len++;
                        }
                    } else {
                        len = 0; // Too long
                    }
                }
            }

            if (len > 0) {
                // Write to stdout
                fwrite(output_buffer, 1, len, stdout);
                fflush(stdout);
                ctx->messages_published++;
            }
        }
    }

    fprintf(stderr, "[Output Publisher] Shutting down\n");
    output_publisher_print_stats(ctx);
    return NULL;
}

void output_publisher_print_stats(const output_publisher_context_t* ctx) {
    fprintf(stderr, "\n=== Output Publisher Statistics ===\n");
    fprintf(stderr, "Messages published:    %llu\n", (unsigned long long)ctx->messages_published);
    fprintf(stderr, "  Acks:                %llu\n", (unsigned long long)ctx->acks_published);
    fprintf(stderr, "  Trades:              %llu\n", (unsigned long long)ctx->trades_published);
    fprintf(stderr, "  Cancel Acks:         %llu\n", (unsigned long long)ctx->cancels_published);
    fprintf(stderr, "  TOB Updates:         %llu\n", (unsigned long long)ctx->tob_updates_published);
}

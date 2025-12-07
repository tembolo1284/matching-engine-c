#include "modes/udp_mode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "core/matching_engine.h"
#include "core/order_book.h"
#include "network/udp_receiver.h"
#include "threading/processor.h"
#include "threading/output_publisher.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_formatter.h"

/* External shutdown flag (from main.c) */
extern atomic_bool g_shutdown;

/**
 * Helper function for 64-byte aligned allocation
 * Required because AVX-512 vectorized loops need 64-byte aligned memory
 */
static inline void* aligned_alloc_64(size_t size) {
    void* ptr = NULL;
    if (posix_memalign(&ptr, 64, size) != 0) {
        return NULL;
    }
    return ptr;
}

/**
 * Dual-output publisher thread for UDP dual-processor mode
 * Reads from both output queues in round-robin fashion
 */
typedef struct {
    output_envelope_queue_t* queue_0;
    output_envelope_queue_t* queue_1;
    bool use_binary_output;
    bool quiet_mode;
    atomic_bool* shutdown_flag;
    /* Statistics */
    uint64_t messages_published;
    uint64_t acks_published;
    uint64_t trades_published;
    uint64_t cancels_published;
    uint64_t tob_updates_published;
} dual_output_publisher_ctx_t;

static void dual_output_publisher_print_stats(const dual_output_publisher_ctx_t* ctx) {
    fprintf(stderr, "\n=== Dual Output Publisher Statistics ===\n");
    fprintf(stderr, "Messages published:    %llu\n", (unsigned long long)ctx->messages_published);
    fprintf(stderr, "  Acks:                %llu\n", (unsigned long long)ctx->acks_published);
    fprintf(stderr, "  Trades:              %llu\n", (unsigned long long)ctx->trades_published);
    fprintf(stderr, "  Cancel Acks:         %llu\n", (unsigned long long)ctx->cancels_published);
    fprintf(stderr, "  TOB Updates:         %llu\n", (unsigned long long)ctx->tob_updates_published);
}

static inline void track_message_type(dual_output_publisher_ctx_t* ctx, output_msg_t* msg) {
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
    ctx->messages_published++;
}

static void* dual_output_publisher_thread(void* arg) {
    dual_output_publisher_ctx_t* ctx = (dual_output_publisher_ctx_t*)arg;

    fprintf(stderr, "[Dual Output Publisher] Starting (format: %s, quiet: %s)\n",
            ctx->use_binary_output ? "Binary" : "CSV",
            ctx->quiet_mode ? "yes" : "no");

    output_msg_envelope_t envelope;

    /* Formatter state (one instance per thread) */
    binary_message_formatter_t bin_formatter;
    message_formatter_t csv_formatter;
    memset(&bin_formatter, 0, sizeof(bin_formatter));
    memset(&csv_formatter, 0, sizeof(csv_formatter));

    /* Progress tracking for quiet mode */
    uint64_t last_progress_time = 0;
    uint64_t last_progress_msgs = 0;
    const uint64_t PROGRESS_INTERVAL_NS = 30ULL * 1000000000ULL;  /* 10 seconds */
    
    /* Get start time */
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    uint64_t start_time_ns = (uint64_t)ts_start.tv_sec * 1000000000ULL + ts_start.tv_nsec;
    last_progress_time = start_time_ns;

    while (!atomic_load(ctx->shutdown_flag)) {
        bool got_message = false;

        /* Round-robin: Try queue 0, then queue 1 */
        if (output_envelope_queue_dequeue(ctx->queue_0, &envelope)) {
            track_message_type(ctx, &envelope.msg);

            if (!ctx->quiet_mode) {
                if (ctx->use_binary_output) {
                    size_t len = 0;
                    const void* data = binary_message_formatter_format(
                        &bin_formatter,
                        &envelope.msg,
                        &len
                    );
                    if (data && len > 0) {
                        fwrite(data, 1, len, stdout);
                        fflush(stdout);
                    }
                } else {
                    const char* line = message_formatter_format(
                        &csv_formatter,
                        &envelope.msg
                    );
                    if (line) {
                        fputs(line, stdout);
                        fflush(stdout);
                    }
                }
            }
            got_message = true;
        }

        if (output_envelope_queue_dequeue(ctx->queue_1, &envelope)) {
            track_message_type(ctx, &envelope.msg);

            if (!ctx->quiet_mode) {
                if (ctx->use_binary_output) {
                    size_t len = 0;
                    const void* data = binary_message_formatter_format(
                        &bin_formatter,
                        &envelope.msg,
                        &len
                    );
                    if (data && len > 0) {
                        fwrite(data, 1, len, stdout);
                        fflush(stdout);
                    }
                } else {
                    const char* line = message_formatter_format(
                        &csv_formatter,
                        &envelope.msg
                    );
                    if (line) {
                        fputs(line, stdout);
                        fflush(stdout);
                    }
                }
            }
            got_message = true;
        }

        if (!got_message) {
            /* Both queues empty, brief sleep */
            struct timespec ts = {0, 1000};  /* 1μs */
            nanosleep(&ts, NULL);
        }

        /* Periodic progress update in quiet mode */
        if (ctx->quiet_mode) {
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            uint64_t now_ns = (uint64_t)ts_now.tv_sec * 1000000000ULL + ts_now.tv_nsec;
            
            if (now_ns - last_progress_time >= PROGRESS_INTERVAL_NS) {
                uint64_t elapsed_ns = now_ns - start_time_ns;
                double elapsed_sec = (double)elapsed_ns / 1e9;
                uint64_t msgs_since_last = ctx->messages_published - last_progress_msgs;
                double interval_sec = (double)(now_ns - last_progress_time) / 1e9;
                double current_rate = (interval_sec > 0) ? (msgs_since_last / interval_sec) : 0;
                double avg_rate = (elapsed_sec > 0) ? (ctx->messages_published / elapsed_sec) : 0;
                
                fprintf(stderr, "[PROGRESS] %6.1fs | %12llu msgs | %10llu trades | %8.2fK msg/s (avg: %.2fK)\n",
                        elapsed_sec,
                        (unsigned long long)ctx->messages_published,
                        (unsigned long long)ctx->trades_published,
                        current_rate / 1000.0,
                        avg_rate / 1000.0);
                
                last_progress_time = now_ns;
                last_progress_msgs = ctx->messages_published;
            }
        }
    }

    fprintf(stderr, "[Dual Output Publisher] Shutting down\n");
    dual_output_publisher_print_stats(ctx);
    return NULL;
}

/**
 * UDP Mode - Dual Processor Implementation
 */
int run_udp_dual_processor(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - UDP Mode (Dual Processor)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "Quiet mode:     %s\n", config->quiet_mode ? "Yes (benchmark)" : "No");
    fprintf(stderr, "Processors:     2 (A-M, N-Z)\n");
    fprintf(stderr, "Output:         Both processors (round-robin)\n");
    fprintf(stderr, "========================================\n");

    /* Initialize memory pools (one per processor) */
    /* Use 64-byte aligned allocation for AVX-512 compatibility */
    memory_pools_t* pools_0 = aligned_alloc_64(sizeof(memory_pools_t));
    memory_pools_t* pools_1 = aligned_alloc_64(sizeof(memory_pools_t));

    if (!pools_0 || !pools_1) {
        fprintf(stderr, "[UDP Dual] Failed to allocate memory pools\n");
        free(pools_0);
        free(pools_1);
        return 1;
    }
    memory_pools_init(pools_0);
    memory_pools_init(pools_1);

    fprintf(stderr, "\nMemory Pools Initialized (per processor):\n");
    fprintf(stderr, "  Order pool:      %d slots each\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash table:       %d slots (open-addressing)\n", ORDER_MAP_SIZE);
    fprintf(stderr, "========================================\n\n");

    /* Initialize matching engines */
    matching_engine_t* engine_0 = malloc(sizeof(matching_engine_t));
    matching_engine_t* engine_1 = malloc(sizeof(matching_engine_t));
    if (!engine_0 || !engine_1) {
        fprintf(stderr, "[UDP Dual] Failed to allocate matching engines\n");
        free(engine_0);
        free(engine_1);
        free(pools_0);
        free(pools_1);
        return 1;
    }
    matching_engine_init(engine_0, pools_0);
    matching_engine_init(engine_1, pools_1);

    /* Initialize queues */
    input_envelope_queue_t* input_queue_0 = malloc(sizeof(input_envelope_queue_t));
    input_envelope_queue_t* input_queue_1 = malloc(sizeof(input_envelope_queue_t));
    output_envelope_queue_t* output_queue_0 = malloc(sizeof(output_envelope_queue_t));
    output_envelope_queue_t* output_queue_1 = malloc(sizeof(output_envelope_queue_t));

    if (!input_queue_0 || !input_queue_1 || !output_queue_0 || !output_queue_1) {
        fprintf(stderr, "[UDP Dual] Failed to allocate queues\n");
        free(input_queue_0);
        free(input_queue_1);
        free(output_queue_0);
        free(output_queue_1);
        matching_engine_destroy(engine_0);
        matching_engine_destroy(engine_1);
        free(engine_0);
        free(engine_1);
        free(pools_0);
        free(pools_1);
        return 1;
    }

    input_envelope_queue_init(input_queue_0);
    input_envelope_queue_init(input_queue_1);
    output_envelope_queue_init(output_queue_0);
    output_envelope_queue_init(output_queue_1);

    /* Thread contexts */
    udp_receiver_t receiver_ctx;
    processor_t processor_ctx_0;
    processor_t processor_ctx_1;
    dual_output_publisher_ctx_t dual_publisher_ctx;

    /* Initialize UDP receiver (dual mode) */
    udp_receiver_init_dual(&receiver_ctx, input_queue_0, input_queue_1, config->port);

    /* Configure processors - each needs its own config with correct processor_id */
    processor_config_t processor_config_0 = {
        .processor_id = 0,
        .tcp_mode = false
    };

    processor_config_t processor_config_1 = {
        .processor_id = 1,
        .tcp_mode = false
    };

    if (!processor_init(&processor_ctx_0, &processor_config_0, engine_0,
                        input_queue_0, output_queue_0, &g_shutdown)) {
        fprintf(stderr, "[UDP Dual] Failed to initialize processor 0\n");
        goto cleanup;
    }

    if (!processor_init(&processor_ctx_1, &processor_config_1, engine_1,
                        input_queue_1, output_queue_1, &g_shutdown)) {
        fprintf(stderr, "[UDP Dual] Failed to initialize processor 1\n");
        goto cleanup;
    }

    /* Configure dual output publisher (reads from BOTH queues!) */
    memset(&dual_publisher_ctx, 0, sizeof(dual_publisher_ctx));
    dual_publisher_ctx.queue_0 = output_queue_0;
    dual_publisher_ctx.queue_1 = output_queue_1;
    dual_publisher_ctx.use_binary_output = config->binary_output;
    dual_publisher_ctx.quiet_mode = config->quiet_mode;
    dual_publisher_ctx.shutdown_flag = &g_shutdown;

    /* Create threads */
    pthread_t processor_0_tid, processor_1_tid, publisher_tid;

    /* Start UDP receiver */
    if (!udp_receiver_start(&receiver_ctx)) {
        fprintf(stderr, "[UDP Dual] Failed to start UDP receiver\n");
        goto cleanup;
    }

    if (pthread_create(&processor_0_tid, NULL, processor_thread, &processor_ctx_0) != 0) {
        fprintf(stderr, "[UDP Dual] Failed to create processor 0 thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        goto cleanup;
    }

    if (pthread_create(&processor_1_tid, NULL, processor_thread, &processor_ctx_1) != 0) {
        fprintf(stderr, "[UDP Dual] Failed to create processor 1 thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        pthread_join(processor_0_tid, NULL);
        goto cleanup;
    }

    if (pthread_create(&publisher_tid, NULL, dual_output_publisher_thread, &dual_publisher_ctx) != 0) {
        fprintf(stderr, "[UDP Dual] Failed to create dual output publisher thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        pthread_join(processor_0_tid, NULL);
        pthread_join(processor_1_tid, NULL);
        goto cleanup;
    }

    fprintf(stderr, "✓ All threads started successfully (4 threads)\n");
    fprintf(stderr, "  - UDP Receiver (routes by symbol)\n");
    fprintf(stderr, "  - Processor 0 (symbols A-M)\n");
    fprintf(stderr, "  - Processor 1 (symbols N-Z)\n");
    fprintf(stderr, "  - Dual Output Publisher (round-robin from both)\n");
    fprintf(stderr, "✓ Matching engine ready\n\n");

    /* Wait for shutdown signal */
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    /* Graceful shutdown */
    fprintf(stderr, "\n[UDP Dual] Initiating graceful shutdown...\n");

    udp_receiver_stop(&receiver_ctx);

    fprintf(stderr, "[UDP Dual] Draining queues...\n");
    sleep(2);

    fprintf(stderr, "[UDP Dual] Waiting for threads to finish...\n");
    pthread_join(processor_0_tid, NULL);
    pthread_join(processor_1_tid, NULL);
    pthread_join(publisher_tid, NULL);

    /* Print statistics */
    fprintf(stderr, "\n--- Processor 0 (A-M) ---\n");
    processor_print_stats(&processor_ctx_0);
    print_memory_stats("Processor 0 (A-M)", pools_0);

    fprintf(stderr, "\n--- Processor 1 (N-Z) ---\n");
    processor_print_stats(&processor_ctx_1);
    print_memory_stats("Processor 1 (N-Z)", pools_1);

cleanup:
    udp_receiver_destroy(&receiver_ctx);

    input_envelope_queue_destroy(input_queue_0);
    input_envelope_queue_destroy(input_queue_1);
    output_envelope_queue_destroy(output_queue_0);
    output_envelope_queue_destroy(output_queue_1);
    free(input_queue_0);
    free(input_queue_1);
    free(output_queue_0);
    free(output_queue_1);

    matching_engine_destroy(engine_0);
    matching_engine_destroy(engine_1);
    free(engine_0);
    free(engine_1);

    free(pools_0);
    free(pools_1);

    fprintf(stderr, "\n=== UDP Dual Processor Mode Stopped ===\n");
    return 0;
}

/**
 * UDP Mode - Single Processor Implementation
 */
int run_udp_single_processor(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - UDP Mode (Single Processor)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Output format:  %s\n", config->binary_output ? "Binary" : "CSV");
    fprintf(stderr, "Quiet mode:     %s\n", config->quiet_mode ? "Yes (benchmark)" : "No");
    fprintf(stderr, "========================================\n");

    /* Initialize memory pools */
    /* Use 64-byte aligned allocation for AVX-512 compatibility */
    memory_pools_t* pools = aligned_alloc_64(sizeof(memory_pools_t));
    if (!pools) {
        fprintf(stderr, "[UDP Single] Failed to allocate memory pools\n");
        return 1;
    }
    memory_pools_init(pools);

    fprintf(stderr, "\nMemory Pools Initialized:\n");
    fprintf(stderr, "  Order pool:      %d slots\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash table:       %d slots (open-addressing)\n", ORDER_MAP_SIZE);
    fprintf(stderr, "========================================\n\n");

    /* Initialize matching engine */
    matching_engine_t* engine = malloc(sizeof(matching_engine_t));
    if (!engine) {
        fprintf(stderr, "[UDP Single] Failed to allocate matching engine\n");
        free(pools);
        return 1;
    }
    matching_engine_init(engine, pools);

    /* Initialize queues */
    input_envelope_queue_t* input_queue = malloc(sizeof(input_envelope_queue_t));
    output_envelope_queue_t* output_queue = malloc(sizeof(output_envelope_queue_t));
    if (!input_queue || !output_queue) {
        fprintf(stderr, "[UDP Single] Failed to allocate queues\n");
        free(input_queue);
        free(output_queue);
        matching_engine_destroy(engine);
        free(engine);
        free(pools);
        return 1;
    }
    input_envelope_queue_init(input_queue);
    output_envelope_queue_init(output_queue);

    /* Thread contexts */
    udp_receiver_t receiver_ctx;
    processor_t processor_ctx;
    output_publisher_context_t publisher_ctx;

    /* Initialize UDP receiver (single mode) */
    udp_receiver_init(&receiver_ctx, input_queue, config->port);

    /* Configure processor */
    processor_config_t processor_config = {
        .processor_id = 0,
        .tcp_mode = false
    };

    if (!processor_init(&processor_ctx, &processor_config, engine,
                        input_queue, output_queue, &g_shutdown)) {
        fprintf(stderr, "[UDP Single] Failed to initialize processor\n");
        goto cleanup;
    }

    /* Configure output publisher */
    output_publisher_config_t publisher_config = {
        .use_binary_output = config->binary_output,
        .quiet_mode = config->quiet_mode
    };

    if (!output_publisher_init(&publisher_ctx, &publisher_config, output_queue, &g_shutdown)) {
        fprintf(stderr, "[UDP Single] Failed to initialize output publisher\n");
        goto cleanup;
    }

    /* Create threads */
    pthread_t processor_tid, publisher_tid;

    /* Start UDP receiver */
    if (!udp_receiver_start(&receiver_ctx)) {
        fprintf(stderr, "[UDP Single] Failed to start UDP receiver\n");
        goto cleanup;
    }

    if (pthread_create(&processor_tid, NULL, processor_thread, &processor_ctx) != 0) {
        fprintf(stderr, "[UDP Single] Failed to create processor thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        goto cleanup;
    }

    if (pthread_create(&publisher_tid, NULL, output_publisher_thread, &publisher_ctx) != 0) {
        fprintf(stderr, "[UDP Single] Failed to create output publisher thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(&receiver_ctx);
        pthread_join(processor_tid, NULL);
        goto cleanup;
    }

    fprintf(stderr, "✓ All threads started successfully\n");
    fprintf(stderr, "✓ Matching engine ready\n\n");

    /* Wait for shutdown signal */
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    /* Graceful shutdown */
    fprintf(stderr, "\n[UDP Single] Initiating graceful shutdown...\n");

    udp_receiver_stop(&receiver_ctx);

    fprintf(stderr, "[UDP Single] Draining queues...\n");
    sleep(2);

    fprintf(stderr, "[UDP Single] Waiting for threads to finish...\n");
    pthread_join(processor_tid, NULL);
    pthread_join(publisher_tid, NULL);

    print_memory_stats("Single Processor", pools);

cleanup:
    udp_receiver_destroy(&receiver_ctx);

    input_envelope_queue_destroy(input_queue);
    output_envelope_queue_destroy(output_queue);
    free(input_queue);
    free(output_queue);

    matching_engine_destroy(engine);
    free(engine);

    free(pools);

    fprintf(stderr, "\n=== UDP Single Processor Mode Stopped ===\n");
    return 0;
}

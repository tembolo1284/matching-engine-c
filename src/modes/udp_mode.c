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
 */
static inline void* aligned_alloc_64(size_t size) {
    void* ptr = NULL;
    if (posix_memalign(&ptr, 64, size) != 0) {
        return NULL;
    }
    return ptr;
}

/* ============================================================================
 * UDP Output Publisher - Sends responses back to clients via UDP
 * ============================================================================ */

/**
 * UDP Output Publisher context
 * Reads from output queues and sends responses back via UDP
 */
typedef struct {
    /* Output queues to drain */
    output_envelope_queue_t* queue_0;
    output_envelope_queue_t* queue_1;
    
    /* UDP receiver for sending responses */
    udp_receiver_t* udp;
    
    /* Configuration */
    bool also_write_stdout;     /* Also write to stdout (for debugging) */
    bool quiet_mode;            /* Suppress stdout, only periodic stats */
    atomic_bool* shutdown_flag;
    
    /* Formatters */
    binary_message_formatter_t bin_formatter;
    message_formatter_t csv_formatter;
    
    /* Statistics */
    uint64_t messages_sent;
    uint64_t acks_sent;
    uint64_t trades_sent;
    uint64_t cancels_sent;
    uint64_t tob_updates_sent;
    uint64_t send_failures;
    uint64_t broadcast_messages;
} udp_output_publisher_ctx_t;

static void udp_output_publisher_print_stats(const udp_output_publisher_ctx_t* ctx) {
    fprintf(stderr, "\n=== UDP Output Publisher Statistics ===\n");
    fprintf(stderr, "Messages sent:         %llu\n", (unsigned long long)ctx->messages_sent);
    fprintf(stderr, "  Acks:                %llu\n", (unsigned long long)ctx->acks_sent);
    fprintf(stderr, "  Trades:              %llu\n", (unsigned long long)ctx->trades_sent);
    fprintf(stderr, "  Cancel Acks:         %llu\n", (unsigned long long)ctx->cancels_sent);
    fprintf(stderr, "  TOB Updates:         %llu\n", (unsigned long long)ctx->tob_updates_sent);
    fprintf(stderr, "Send failures:         %llu\n", (unsigned long long)ctx->send_failures);
    fprintf(stderr, "Broadcast messages:    %llu\n", (unsigned long long)ctx->broadcast_messages);
}

static inline void track_output_type(udp_output_publisher_ctx_t* ctx, const output_msg_t* msg) {
    switch (msg->type) {
        case OUTPUT_MSG_ACK:
            ctx->acks_sent++;
            break;
        case OUTPUT_MSG_TRADE:
            ctx->trades_sent++;
            break;
        case OUTPUT_MSG_CANCEL_ACK:
            ctx->cancels_sent++;
            break;
        case OUTPUT_MSG_TOP_OF_BOOK:
            ctx->tob_updates_sent++;
            break;
        default:
            break;
    }
    ctx->messages_sent++;
}

/**
 * Send output message to client via UDP
 * Automatically formats as binary or CSV based on client protocol
 */
static bool send_output_to_client(udp_output_publisher_ctx_t* ctx,
                                   const output_msg_envelope_t* envelope) {
    uint32_t client_id = envelope->client_id;
    const output_msg_t* msg = &envelope->msg;
    
    /* Determine format based on client protocol */
    client_protocol_t protocol = CLIENT_PROTOCOL_CSV;  /* Default to CSV */
    
    if (client_id_is_udp(client_id)) {
        protocol = udp_receiver_get_client_protocol(ctx->udp, client_id);
        if (protocol == CLIENT_PROTOCOL_UNKNOWN) {
            protocol = CLIENT_PROTOCOL_CSV;  /* Default */
        }
    }
    
    /* Format message */
    const void* data;
    size_t len;
    
    if (protocol == CLIENT_PROTOCOL_BINARY) {
        data = binary_message_formatter_format(&ctx->bin_formatter, msg, &len);
    } else {
        const char* line = message_formatter_format(&ctx->csv_formatter, msg);
        if (line) {
            data = line;
            len = strlen(line);
        } else {
            data = NULL;
            len = 0;
        }
    }
    
    if (!data || len == 0) {
        return false;
    }
    
    /* Send to client */
    bool success = false;
    
    if (client_id == CLIENT_ID_BROADCAST) {
        /* Broadcast - for now, skip (would need multicast or client list iteration) */
        ctx->broadcast_messages++;
        success = true;  /* Don't count as failure */
    } else if (client_id_is_udp(client_id)) {
        success = udp_receiver_send(ctx->udp, client_id, data, len);
    }
    /* TCP clients would be handled here if we had TCP support */
    
    if (!success && client_id != CLIENT_ID_BROADCAST) {
        ctx->send_failures++;
    }
    
    return success;
}

/**
 * Process a single output envelope
 */
static void process_output_envelope(udp_output_publisher_ctx_t* ctx,
                                     const output_msg_envelope_t* envelope) {
    track_output_type(ctx, &envelope->msg);
    
    /* Send via UDP to client */
    send_output_to_client(ctx, envelope);
    
    /* Also write to stdout if requested (for debugging/compatibility) */
    if (ctx->also_write_stdout && !ctx->quiet_mode) {
        client_protocol_t protocol = CLIENT_PROTOCOL_CSV;
        if (client_id_is_udp(envelope->client_id)) {
            protocol = udp_receiver_get_client_protocol(ctx->udp, envelope->client_id);
        }
        
        if (protocol == CLIENT_PROTOCOL_BINARY) {
            size_t len = 0;
            const void* data = binary_message_formatter_format(
                &ctx->bin_formatter, &envelope->msg, &len);
            if (data && len > 0) {
                fwrite(data, 1, len, stdout);
            }
        } else {
            const char* line = message_formatter_format(
                &ctx->csv_formatter, &envelope->msg);
            if (line) {
                fputs(line, stdout);
            }
        }
        fflush(stdout);
    }
}

/**
 * UDP Output Publisher thread - dual queue version
 */
static void* udp_output_publisher_thread(void* arg) {
    udp_output_publisher_ctx_t* ctx = (udp_output_publisher_ctx_t*)arg;
    
    fprintf(stderr, "[UDP Output Publisher] Starting (quiet: %s, stdout: %s)\n",
            ctx->quiet_mode ? "yes" : "no",
            ctx->also_write_stdout ? "yes" : "no");
    
    output_msg_envelope_t envelope;
    
    /* Progress tracking for quiet mode */
    uint64_t last_progress_time = 0;
    uint64_t last_progress_msgs = 0;
    const uint64_t PROGRESS_INTERVAL_NS = 10ULL * 1000000000ULL;  /* 10 seconds */
    
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    uint64_t start_time_ns = (uint64_t)ts_start.tv_sec * 1000000000ULL + ts_start.tv_nsec;
    last_progress_time = start_time_ns;
    
    while (!atomic_load(ctx->shutdown_flag)) {
        bool got_message = false;
        
        /* Drain both queues in round-robin fashion */
        if (output_envelope_queue_dequeue(ctx->queue_0, &envelope)) {
            process_output_envelope(ctx, &envelope);
            got_message = true;
        }
        
        if (ctx->queue_1 && output_envelope_queue_dequeue(ctx->queue_1, &envelope)) {
            process_output_envelope(ctx, &envelope);
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
                uint64_t msgs_since_last = ctx->messages_sent - last_progress_msgs;
                double interval_sec = (double)(now_ns - last_progress_time) / 1e9;
                double current_rate = (interval_sec > 0) ? (msgs_since_last / interval_sec) : 0;
                double avg_rate = (elapsed_sec > 0) ? (ctx->messages_sent / elapsed_sec) : 0;
                
                fprintf(stderr, "[PROGRESS] %6.1fs | %12llu sent | %10llu trades | %8.2fK msg/s (avg: %.2fK) | %u clients\n",
                        elapsed_sec,
                        (unsigned long long)ctx->messages_sent,
                        (unsigned long long)ctx->trades_sent,
                        current_rate / 1000.0,
                        avg_rate / 1000.0,
                        udp_receiver_get_client_count(ctx->udp));
                
                last_progress_time = now_ns;
                last_progress_msgs = ctx->messages_sent;
            }
        }
    }
    
    fprintf(stderr, "[UDP Output Publisher] Shutting down\n");
    udp_output_publisher_print_stats(ctx);
    return NULL;
}

/**
 * UDP Output Publisher thread - single queue version
 */
static void* udp_output_publisher_single_thread(void* arg) {
    udp_output_publisher_ctx_t* ctx = (udp_output_publisher_ctx_t*)arg;
    
    fprintf(stderr, "[UDP Output Publisher] Starting single-queue mode\n");
    
    output_msg_envelope_t envelope;
    
    /* Progress tracking */
    uint64_t last_progress_time = 0;
    uint64_t last_progress_msgs = 0;
    const uint64_t PROGRESS_INTERVAL_NS = 10ULL * 1000000000ULL;
    
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    uint64_t start_time_ns = (uint64_t)ts_start.tv_sec * 1000000000ULL + ts_start.tv_nsec;
    last_progress_time = start_time_ns;
    
    while (!atomic_load(ctx->shutdown_flag)) {
        if (output_envelope_queue_dequeue(ctx->queue_0, &envelope)) {
            process_output_envelope(ctx, &envelope);
        } else {
            struct timespec ts = {0, 1000};
            nanosleep(&ts, NULL);
        }
        
        /* Periodic progress */
        if (ctx->quiet_mode) {
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            uint64_t now_ns = (uint64_t)ts_now.tv_sec * 1000000000ULL + ts_now.tv_nsec;
            
            if (now_ns - last_progress_time >= PROGRESS_INTERVAL_NS) {
                uint64_t elapsed_ns = now_ns - start_time_ns;
                double elapsed_sec = (double)elapsed_ns / 1e9;
                uint64_t msgs_since_last = ctx->messages_sent - last_progress_msgs;
                double interval_sec = (double)(now_ns - last_progress_time) / 1e9;
                double current_rate = (interval_sec > 0) ? (msgs_since_last / interval_sec) : 0;
                double avg_rate = (elapsed_sec > 0) ? (ctx->messages_sent / elapsed_sec) : 0;
                
                fprintf(stderr, "[PROGRESS] %6.1fs | %12llu sent | %8.2fK msg/s (avg: %.2fK) | %u clients\n",
                        elapsed_sec,
                        (unsigned long long)ctx->messages_sent,
                        current_rate / 1000.0,
                        avg_rate / 1000.0,
                        udp_receiver_get_client_count(ctx->udp));
                
                last_progress_time = now_ns;
                last_progress_msgs = ctx->messages_sent;
            }
        }
    }
    
    fprintf(stderr, "[UDP Output Publisher] Shutting down\n");
    udp_output_publisher_print_stats(ctx);
    return NULL;
}

/* ============================================================================
 * UDP Mode - Dual Processor (Bidirectional)
 * ============================================================================ */

int run_udp_dual_processor(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - UDP Mode (Dual Processor, Bidirectional)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Protocol:       Auto-detect (Binary/CSV per client)\n");
    fprintf(stderr, "Quiet mode:     %s\n", config->quiet_mode ? "Yes (benchmark)" : "No");
    fprintf(stderr, "Processors:     2 (A-M, N-Z)\n");
    fprintf(stderr, "Response mode:  UDP (back to client)\n");
    fprintf(stderr, "========================================\n");

    int ret = 1;
    
    /* Initialize memory pools (one per processor) */
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
    fprintf(stderr, "  Order pool:       %d slots each\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash table:       %d slots (open-addressing)\n", ORDER_MAP_SIZE);
    fprintf(stderr, "  UDP clients:      %d max tracked\n", MAX_UDP_CLIENTS);
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
        goto cleanup_queues;
    }

    input_envelope_queue_init(input_queue_0);
    input_envelope_queue_init(input_queue_1);
    output_envelope_queue_init(output_queue_0);
    output_envelope_queue_init(output_queue_1);

    /* Thread contexts */
    udp_receiver_t* receiver_ctx = malloc(sizeof(udp_receiver_t));
    if (!receiver_ctx) {
        fprintf(stderr, "[UDP Dual] Failed to allocate receiver context\n");
        goto cleanup_queues;
    }
    
    processor_t processor_ctx_0;
    processor_t processor_ctx_1;
    udp_output_publisher_ctx_t publisher_ctx;

    /* Initialize UDP receiver (dual mode, bidirectional) */
    udp_receiver_init_dual(receiver_ctx, input_queue_0, input_queue_1, config->port);

    /* Configure processors */
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
        goto cleanup_receiver;
    }

    if (!processor_init(&processor_ctx_1, &processor_config_1, engine_1,
                        input_queue_1, output_queue_1, &g_shutdown)) {
        fprintf(stderr, "[UDP Dual] Failed to initialize processor 1\n");
        goto cleanup_receiver;
    }

    /* Configure UDP output publisher */
    memset(&publisher_ctx, 0, sizeof(publisher_ctx));
    publisher_ctx.queue_0 = output_queue_0;
    publisher_ctx.queue_1 = output_queue_1;
    publisher_ctx.udp = receiver_ctx;
    publisher_ctx.also_write_stdout = false;  /* Set true for debugging */
    publisher_ctx.quiet_mode = config->quiet_mode;
    publisher_ctx.shutdown_flag = &g_shutdown;
    binary_message_formatter_init(&publisher_ctx.bin_formatter);
    memset(&publisher_ctx.csv_formatter, 0, sizeof(publisher_ctx.csv_formatter));

    /* Create threads */
    pthread_t processor_0_tid, processor_1_tid, publisher_tid;

    /* Start UDP receiver */
    if (!udp_receiver_start(receiver_ctx)) {
        fprintf(stderr, "[UDP Dual] Failed to start UDP receiver\n");
        goto cleanup_receiver;
    }

    if (pthread_create(&processor_0_tid, NULL, processor_thread, &processor_ctx_0) != 0) {
        fprintf(stderr, "[UDP Dual] Failed to create processor 0 thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(receiver_ctx);
        goto cleanup_receiver;
    }

    if (pthread_create(&processor_1_tid, NULL, processor_thread, &processor_ctx_1) != 0) {
        fprintf(stderr, "[UDP Dual] Failed to create processor 1 thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(receiver_ctx);
        pthread_join(processor_0_tid, NULL);
        goto cleanup_receiver;
    }

    if (pthread_create(&publisher_tid, NULL, udp_output_publisher_thread, &publisher_ctx) != 0) {
        fprintf(stderr, "[UDP Dual] Failed to create UDP output publisher thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(receiver_ctx);
        pthread_join(processor_0_tid, NULL);
        pthread_join(processor_1_tid, NULL);
        goto cleanup_receiver;
    }

    fprintf(stderr, "✓ All threads started successfully (4 threads)\n");
    fprintf(stderr, "  - UDP Server (receive + send, routes by symbol)\n");
    fprintf(stderr, "  - Processor 0 (symbols A-M)\n");
    fprintf(stderr, "  - Processor 1 (symbols N-Z)\n");
    fprintf(stderr, "  - UDP Output Publisher (responses to clients)\n");
    fprintf(stderr, "✓ Matching engine ready - accepting connections on port %u\n\n", config->port);

    /* Wait for shutdown signal */
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    /* Graceful shutdown */
    fprintf(stderr, "\n[UDP Dual] Initiating graceful shutdown...\n");

    udp_receiver_stop(receiver_ctx);

    fprintf(stderr, "[UDP Dual] Draining queues...\n");
    usleep(500000);  /* 500ms to drain */

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

    ret = 0;

cleanup_receiver:
    udp_receiver_destroy(receiver_ctx);
    free(receiver_ctx);

cleanup_queues:
    if (input_queue_0) {
        input_envelope_queue_destroy(input_queue_0);
        free(input_queue_0);
    }
    if (input_queue_1) {
        input_envelope_queue_destroy(input_queue_1);
        free(input_queue_1);
    }
    if (output_queue_0) {
        output_envelope_queue_destroy(output_queue_0);
        free(output_queue_0);
    }
    if (output_queue_1) {
        output_envelope_queue_destroy(output_queue_1);
        free(output_queue_1);
    }

    matching_engine_destroy(engine_0);
    matching_engine_destroy(engine_1);
    free(engine_0);
    free(engine_1);

    free(pools_0);
    free(pools_1);

    fprintf(stderr, "\n=== UDP Dual Processor Mode Stopped ===\n");
    return ret;
}

/* ============================================================================
 * UDP Mode - Single Processor (Bidirectional)
 * ============================================================================ */

int run_udp_single_processor(const app_config_t* config) {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Matching Engine - UDP Mode (Single Processor, Bidirectional)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:           %u\n", config->port);
    fprintf(stderr, "Protocol:       Auto-detect (Binary/CSV per client)\n");
    fprintf(stderr, "Quiet mode:     %s\n", config->quiet_mode ? "Yes (benchmark)" : "No");
    fprintf(stderr, "Response mode:  UDP (back to client)\n");
    fprintf(stderr, "========================================\n");

    int ret = 1;

    /* Initialize memory pools */
    memory_pools_t* pools = aligned_alloc_64(sizeof(memory_pools_t));
    if (!pools) {
        fprintf(stderr, "[UDP Single] Failed to allocate memory pools\n");
        return 1;
    }
    memory_pools_init(pools);

    fprintf(stderr, "\nMemory Pools Initialized:\n");
    fprintf(stderr, "  Order pool:       %d slots\n", MAX_ORDERS_IN_POOL);
    fprintf(stderr, "  Hash table:       %d slots (open-addressing)\n", ORDER_MAP_SIZE);
    fprintf(stderr, "  UDP clients:      %d max tracked\n", MAX_UDP_CLIENTS);
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
        goto cleanup_queues;
    }
    input_envelope_queue_init(input_queue);
    output_envelope_queue_init(output_queue);

    /* Thread contexts */
    udp_receiver_t* receiver_ctx = malloc(sizeof(udp_receiver_t));
    if (!receiver_ctx) {
        fprintf(stderr, "[UDP Single] Failed to allocate receiver context\n");
        goto cleanup_queues;
    }
    
    processor_t processor_ctx;
    udp_output_publisher_ctx_t publisher_ctx;

    /* Initialize UDP receiver (single mode, bidirectional) */
    udp_receiver_init(receiver_ctx, input_queue, config->port);

    /* Configure processor */
    processor_config_t processor_config = {
        .processor_id = 0,
        .tcp_mode = false
    };

    if (!processor_init(&processor_ctx, &processor_config, engine,
                        input_queue, output_queue, &g_shutdown)) {
        fprintf(stderr, "[UDP Single] Failed to initialize processor\n");
        goto cleanup_receiver;
    }

    /* Configure UDP output publisher */
    memset(&publisher_ctx, 0, sizeof(publisher_ctx));
    publisher_ctx.queue_0 = output_queue;
    publisher_ctx.queue_1 = NULL;  /* Single queue mode */
    publisher_ctx.udp = receiver_ctx;
    publisher_ctx.also_write_stdout = false;
    publisher_ctx.quiet_mode = config->quiet_mode;
    publisher_ctx.shutdown_flag = &g_shutdown;
    binary_message_formatter_init(&publisher_ctx.bin_formatter);
    memset(&publisher_ctx.csv_formatter, 0, sizeof(publisher_ctx.csv_formatter));

    /* Create threads */
    pthread_t processor_tid, publisher_tid;

    /* Start UDP receiver */
    if (!udp_receiver_start(receiver_ctx)) {
        fprintf(stderr, "[UDP Single] Failed to start UDP receiver\n");
        goto cleanup_receiver;
    }

    if (pthread_create(&processor_tid, NULL, processor_thread, &processor_ctx) != 0) {
        fprintf(stderr, "[UDP Single] Failed to create processor thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(receiver_ctx);
        goto cleanup_receiver;
    }

    if (pthread_create(&publisher_tid, NULL, udp_output_publisher_single_thread, &publisher_ctx) != 0) {
        fprintf(stderr, "[UDP Single] Failed to create output publisher thread\n");
        atomic_store(&g_shutdown, true);
        udp_receiver_stop(receiver_ctx);
        pthread_join(processor_tid, NULL);
        goto cleanup_receiver;
    }

    fprintf(stderr, "All threads started successfully (3 threads)\n");
    fprintf(stderr, "  - UDP Server (receive + send)\n");
    fprintf(stderr, "  - Processor\n");
    fprintf(stderr, "  - UDP Output Publisher\n");
    fprintf(stderr, "Matching engine ready - accepting connections on port %u\n\n", config->port);

    /* Wait for shutdown signal */
    while (!atomic_load(&g_shutdown)) {
        sleep(1);
    }

    /* Graceful shutdown */
    fprintf(stderr, "\n[UDP Single] Initiating graceful shutdown...\n");

    udp_receiver_stop(receiver_ctx);

    fprintf(stderr, "[UDP Single] Draining queues...\n");
    usleep(500000);

    fprintf(stderr, "[UDP Single] Waiting for threads to finish...\n");
    pthread_join(processor_tid, NULL);
    pthread_join(publisher_tid, NULL);

    processor_print_stats(&processor_ctx);
    print_memory_stats("Single Processor", pools);

    ret = 0;

cleanup_receiver:
    udp_receiver_destroy(receiver_ctx);
    free(receiver_ctx);

cleanup_queues:
    if (input_queue) {
        input_envelope_queue_destroy(input_queue);
        free(input_queue);
    }
    if (output_queue) {
        output_envelope_queue_destroy(output_queue);
        free(output_queue);
    }

    matching_engine_destroy(engine);
    free(engine);
    free(pools);

    fprintf(stderr, "\n=== UDP Single Processor Mode Stopped ===\n");
    return ret;
}

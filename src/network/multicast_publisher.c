#include "network/multicast_publisher.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_formatter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define SLEEP_TIME_NS 1000  // 1 microsecond

static const struct timespec ts = {
    .tv_sec = 0,
    .tv_nsec = SLEEP_TIME_NS
};

/**
 * Setup UDP multicast socket
 */
bool multicast_publisher_setup_socket(multicast_publisher_context_t* ctx) {
    // Create UDP socket
    ctx->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->sockfd < 0) {
        fprintf(stderr, "[Multicast Publisher] ERROR: Failed to create socket: %s\n",
                strerror(errno));
        return false;
    }

    // Set socket to allow multiple processes on same port (optional, for testing)
    int reuse = 1;
    if (setsockopt(ctx->sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        fprintf(stderr, "[Multicast Publisher] WARNING: Failed to set SO_REUSEADDR: %s\n",
                strerror(errno));
    }

    // Set multicast TTL (time-to-live)
    // 1 = same subnet only, 255 = global
    unsigned char ttl = ctx->config.ttl;
    if (setsockopt(ctx->sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        fprintf(stderr, "[Multicast Publisher] WARNING: Failed to set multicast TTL: %s\n",
                strerror(errno));
    }

    // Enable multicast loopback (receive our own packets)
    unsigned char loop = 1;
    if (setsockopt(ctx->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        fprintf(stderr, "[Multicast Publisher] WARNING: Failed to disable loopback: %s\n",
                strerror(errno));
    }

    // Configure multicast group address
    memset(&ctx->mcast_addr, 0, sizeof(ctx->mcast_addr));
    ctx->mcast_addr.sin_family = AF_INET;
    ctx->mcast_addr.sin_port = htons(ctx->config.port);

    if (inet_pton(AF_INET, ctx->config.multicast_group, &ctx->mcast_addr.sin_addr) <= 0) {
        fprintf(stderr, "[Multicast Publisher] ERROR: Invalid multicast group: %s\n",
                ctx->config.multicast_group);
        close(ctx->sockfd);
        ctx->sockfd = -1;
        return false;
    }

    // Verify it's a valid multicast address (224.0.0.0 - 239.255.255.255)
    uint32_t addr = ntohl(ctx->mcast_addr.sin_addr.s_addr);
    if ((addr & 0xF0000000) != 0xE0000000) {
        fprintf(stderr, "[Multicast Publisher] WARNING: Address %s is not in multicast range (224.0.0.0-239.255.255.255)\n",
                ctx->config.multicast_group);
    }

    fprintf(stderr, "[Multicast Publisher] Configured to broadcast to %s:%u\n",
            ctx->config.multicast_group, ctx->config.port);
    fprintf(stderr, "[Multicast Publisher] TTL: %u, Protocol: %s\n",
            ctx->config.ttl, ctx->config.use_binary_output ? "Binary" : "CSV");

    return true;
}

/**
 * Initialize multicast publisher (single processor mode)
 */
bool multicast_publisher_init(multicast_publisher_context_t* ctx,
                              const multicast_publisher_config_t* config,
                              output_envelope_queue_t* input_queue,
                              atomic_bool* shutdown_flag) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->config = *config;
    ctx->input_queues[0] = input_queue;
    ctx->input_queues[1] = NULL;
    ctx->num_input_queues = 1;
    ctx->shutdown_flag = shutdown_flag;
    ctx->sockfd = -1;

    atomic_init(&ctx->packets_sent, 0);
    atomic_init(&ctx->messages_broadcast, 0);
    atomic_init(&ctx->messages_from_processor[0], 0);
    atomic_init(&ctx->messages_from_processor[1], 0);
    atomic_init(&ctx->sequence, 0);
    atomic_init(&ctx->send_errors, 0);

    return multicast_publisher_setup_socket(ctx);
}

/**
 * Initialize multicast publisher for dual-processor mode
 */
bool multicast_publisher_init_dual(multicast_publisher_context_t* ctx,
                                   const multicast_publisher_config_t* config,
                                   output_envelope_queue_t* input_queue_0,
                                   output_envelope_queue_t* input_queue_1,
                                   atomic_bool* shutdown_flag) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->config = *config;
    ctx->input_queues[0] = input_queue_0;
    ctx->input_queues[1] = input_queue_1;
    ctx->num_input_queues = 2;
    ctx->shutdown_flag = shutdown_flag;
    ctx->sockfd = -1;

    atomic_init(&ctx->packets_sent, 0);
    atomic_init(&ctx->messages_broadcast, 0);
    atomic_init(&ctx->messages_from_processor[0], 0);
    atomic_init(&ctx->messages_from_processor[1], 0);
    atomic_init(&ctx->sequence, 0);
    atomic_init(&ctx->send_errors, 0);

    return multicast_publisher_setup_socket(ctx);
}

/**
 * Cleanup multicast publisher context
 */
void multicast_publisher_cleanup(multicast_publisher_context_t* ctx) {
    if (ctx->sockfd >= 0) {
        close(ctx->sockfd);
        ctx->sockfd = -1;
    }
}

/**
 * Format and send a single message via multicast
 * Returns true on success, false on error
 *
 * Uses the newer formatter APIs:
 *   - binary_message_formatter_format(binary_message_formatter_t*, const output_msg_t*, size_t* out_len)
 *   - message_formatter_format(message_formatter_t*, const output_msg_t*)
 */
static bool send_multicast_message(multicast_publisher_context_t* ctx,
                                   const output_msg_t* msg) {
    size_t msg_len = 0;
    const void* payload = NULL;

    if (ctx->config.use_binary_output) {
        // Binary protocol: formatter returns pointer + length via out param
        binary_message_formatter_t formatter;
        memset(&formatter, 0, sizeof(formatter));

        payload = binary_message_formatter_format(&formatter, msg, &msg_len);
        if (!payload || msg_len == 0) {
            fprintf(stderr, "[Multicast Publisher] ERROR: Failed to format binary message\n");
            return false;
        }
    } else {
        // CSV protocol: formatter returns a NUL-terminated string
        message_formatter_t formatter;
        memset(&formatter, 0, sizeof(formatter));

        const char* text = message_formatter_format(&formatter, msg);
        if (!text) {
            fprintf(stderr, "[Multicast Publisher] ERROR: Failed to format CSV message\n");
            return false;
        }

        payload = text;
        msg_len = strlen(text);
    }

    // Send to multicast group
    ssize_t sent = sendto(ctx->sockfd, payload, msg_len, 0,
                          (struct sockaddr*)&ctx->mcast_addr,
                          sizeof(ctx->mcast_addr));

    if (sent < 0) {
        fprintf(stderr, "[Multicast Publisher] ERROR: sendto failed: %s\n",
                strerror(errno));
        atomic_fetch_add(&ctx->send_errors, 1);
        return false;
    }

    if ((size_t)sent != msg_len) {
        fprintf(stderr, "[Multicast Publisher] WARNING: Partial send (%zd/%zu bytes)\n",
                sent, msg_len);
        atomic_fetch_add(&ctx->send_errors, 1);
        return false;
    }

    atomic_fetch_add(&ctx->packets_sent, 1);
    atomic_fetch_add(&ctx->messages_broadcast, 1);

    return true;
}

/**
 * Multicast publisher thread entry point
 */
void* multicast_publisher_thread(void* arg) {
    multicast_publisher_context_t* ctx = (multicast_publisher_context_t*)arg;

    fprintf(stderr, "[Multicast Publisher] Starting (queues: %d, group: %s:%u)\n",
            ctx->num_input_queues,
            ctx->config.multicast_group,
            ctx->config.port);

    output_msg_envelope_t batch[MULTICAST_BATCH_SIZE];

    while (!atomic_load(ctx->shutdown_flag)) {
        size_t total_processed = 0;

        // Round-robin across all input queues (same pattern as output_router)
        // This ensures fairness - no processor can starve another
        for (int q = 0; q < ctx->num_input_queues; q++) {
            output_envelope_queue_t* queue = ctx->input_queues[q];
            if (!queue) continue;

            // Dequeue a batch from this queue
            size_t count = 0;
            for (size_t i = 0; i < MULTICAST_BATCH_SIZE; i++) {
                if (output_envelope_queue_dequeue(queue, &batch[count])) {
                    count++;
                } else {
                    break;
                }
            }

            // Broadcast each message to multicast group
            for (size_t i = 0; i < count; i++) {
                output_msg_envelope_t* envelope = &batch[i];

                // Send to multicast group (all subscribers receive)
                (void)send_multicast_message(ctx, &envelope->msg);

                // Track which processor this came from
                atomic_fetch_add(&ctx->messages_from_processor[q], 1);
            }

            total_processed += count;
        }

        if (total_processed == 0) {
            // No messages from any queue - sleep briefly
            nanosleep(&ts, NULL);
        }
    }

    // Drain remaining messages before shutdown
    fprintf(stderr, "[Multicast Publisher] Draining remaining messages...\n");

    bool has_messages = true;
    int drain_iterations = 0;
    const int MAX_DRAIN_ITERATIONS = 100;

    while (has_messages && drain_iterations < MAX_DRAIN_ITERATIONS) {
        has_messages = false;
        drain_iterations++;

        for (int q = 0; q < ctx->num_input_queues; q++) {
            output_envelope_queue_t* queue = ctx->input_queues[q];
            if (!queue) continue;

            output_msg_envelope_t envelope;
            while (output_envelope_queue_dequeue(queue, &envelope)) {
                has_messages = true;
                (void)send_multicast_message(ctx, &envelope.msg);
                atomic_fetch_add(&ctx->messages_from_processor[q], 1);
            }
        }
    }

    fprintf(stderr, "[Multicast Publisher] Shutting down\n");
    multicast_publisher_print_stats(ctx);

    return NULL;
}

/**
 * Print multicast publisher statistics
 */
void multicast_publisher_print_stats(const multicast_publisher_context_t* ctx) {
    fprintf(stderr, "\n=== Multicast Publisher Statistics ===\n");
    fprintf(stderr, "Multicast group:       %s:%u\n",
            ctx->config.multicast_group, ctx->config.port);
    fprintf(stderr, "Packets sent:          %llu\n",
            (unsigned long long)atomic_load(&ctx->packets_sent));
    fprintf(stderr, "Messages broadcast:    %llu\n",
            (unsigned long long)atomic_load(&ctx->messages_broadcast));
    fprintf(stderr, "Send errors:           %llu\n",
            (unsigned long long)atomic_load(&ctx->send_errors));

    if (ctx->num_input_queues == 2) {
        fprintf(stderr, "From Processor 0 (A-M): %llu\n",
                (unsigned long long)atomic_load(&ctx->messages_from_processor[0]));
        fprintf(stderr, "From Processor 1 (N-Z): %llu\n",
                (unsigned long long)atomic_load(&ctx->messages_from_processor[1]));
    }
}


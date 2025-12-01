/**
 * engine_client.c - High-level matching engine client implementation
 *
 * Design principles (Power of Ten):
 * - No dynamic memory allocation (all buffers pre-allocated)
 * - All loops have explicit upper bounds
 * - All return values checked
 * - Minimal variable scope
 * - Simple control flow (early returns for errors)
 * - No recursion
 */

#include "client/engine_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>

/* Maximum iterations for drain loops - prevents runaway */
#define MAX_DRAIN_ITERATIONS 100
#define MAX_RECV_ATTEMPTS    50
#define MAX_POLL_FDS         2

/* ============================================================
 * Timing
 * ============================================================ */

uint64_t engine_client_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================
 * Internal Helpers
 * ============================================================ */

/**
 * Drain all pending messages from transport buffer.
 * Used after probe to clear any leftover responses.
 *
 * Returns: number of messages drained (0 to MAX_DRAIN_ITERATIONS)
 */
static int drain_transport_buffer(transport_t* transport, int timeout_ms) {
    char buf[4096];
    size_t len = 0;
    int count = 0;
    int empty_polls = 0;

    /* Bounded loop: drain at most MAX_DRAIN_ITERATIONS messages */
    for (int i = 0; i < MAX_DRAIN_ITERATIONS; i++) {
        /* Check for buffered data first (zero timeout) */
        if (transport_has_data(transport)) {
            if (transport_recv(transport, buf, sizeof(buf), &len, 0)) {
                count++;
                empty_polls = 0;
                continue;
            }
        }

        /* Poll socket with timeout */
        if (transport_recv(transport, buf, sizeof(buf), &len, timeout_ms)) {
            count++;
            empty_polls = 0;
            timeout_ms = 10;  /* Shorter timeout after first message */
        } else {
            empty_polls++;
            if (empty_polls >= 3) {
                break;  /* Three consecutive empty polls = done */
            }
            timeout_ms = 10;
        }
    }

    return count;
}

/**
 * Send probe order, detect encoding, flush, drain completely.
 * Returns true if encoding was detected, false on error.
 *
 * Protocol:
 *   1. Send probe order (binary) -> triggers ACK + TOB from server
 *   2. Receive first response -> detect encoding (binary or CSV)
 *   3. Send flush -> cancels probe order, triggers Cancel ACK + TOB
 *   4. Wait and drain ALL remaining responses before returning
 *
 * If no response is received, the server is not running.
 */
static bool probe_server_encoding(engine_client_t* client) {
    client_config_t* cfg = &client->config;
    const void* data = NULL;
    size_t len = 0;
    char recv_buf[4096];
    size_t recv_len = 0;
    bool success = false;

    /* Use binary encoding for probe */
    encoding_type_t saved_encoding = client->codec.send_encoding;
    client->codec.send_encoding = ENCODING_BINARY;

    /* Send probe order - use Z-prefix symbol to route to processor 1 */
    if (!codec_encode_new_order(&client->codec, cfg->user_id, "ZPROBE",
                                1, 1, SIDE_BUY, 1, &data, &len)) {
        goto cleanup;
    }

    if (!transport_send(&client->transport, data, len)) {
        goto cleanup;
    }

    /* Wait for first response to detect encoding */
    if (transport_recv(&client->transport, recv_buf, sizeof(recv_buf),
                       &recv_len, CLIENT_PROBE_TIMEOUT_MS)) {
        cfg->detected_encoding = codec_detect_encoding(recv_buf, recv_len);
        client->codec.detected_encoding = cfg->detected_encoding;
        client->codec.encoding_detected = true;
        success = true;
    } else {
        /* No response - server is not running or not responding */
        fprintf(stderr, "No response from server (is it running?)\n");
        success = false;
        goto cleanup;
    }

    /* Send flush - this cancels the probe order */
    if (codec_encode_flush(&client->codec, &data, &len)) {
        (void)transport_send(&client->transport, data, len);
    }

    /* 
     * CRITICAL: Wait for server to fully process flush across all processors.
     * The dual-processor architecture means messages route through queues
     * and responses may arrive with delay. We must drain everything.
     *
     * Expected responses after flush:
     *   - Remaining TOB from probe order (if not received yet)
     *   - Cancel ACK for probe order (C, ZPROBE, 1, 1)
     *   - TOB update showing empty book (B, ZPROBE, B, -, -)
     *
     * Wait up to 500ms total, checking every 50ms.
     */
    for (int wait = 0; wait < 10; wait++) {
        int drained = drain_transport_buffer(&client->transport, 50);
        if (drained == 0 && wait >= 2) {
            /* No messages for 2+ consecutive waits - we're done */
            break;
        }
    }

cleanup:
    client->codec.send_encoding = saved_encoding;
    return success;
}

/* ============================================================
 * Lifecycle
 * ============================================================ */

void engine_client_init(engine_client_t* client, const client_config_t* config) {
    if (client == NULL || config == NULL) {
        return;
    }

    memset(client, 0, sizeof(*client));
    client->config = *config;

    transport_init(&client->transport);
    codec_init(&client->codec, config->encoding);
    multicast_receiver_init(&client->multicast);

    client->multicast_active = false;
    client->connected = false;
    client->next_order_id = 1;
    client->min_latency = UINT64_MAX;
    client->max_latency = 0;
}

bool engine_client_connect(engine_client_t* client) {
    if (client == NULL) {
        return false;
    }

    client_config_t* cfg = &client->config;

    /* Multicast-only mode needs no TCP/UDP connection */
    if (cfg->mode == MODE_MULTICAST_ONLY) {
        client->connected = true;
        return true;
    }

    if (!cfg->quiet) {
        printf("Connecting to %s:%u...\n", cfg->host, cfg->port);
    }

    /* Connect transport */
    if (!transport_connect(&client->transport, cfg->host, cfg->port,
                           cfg->transport, cfg->connect_timeout_ms)) {
        fprintf(stderr, "Failed to connect to %s:%u\n", cfg->host, cfg->port);
        return false;
    }

    cfg->detected_transport = transport_get_type(&client->transport);

    if (!cfg->quiet) {
        printf("Connected via %s\n", transport_type_str(cfg->detected_transport));
    }

    /* Determine encoding */
    if (cfg->encoding != ENCODING_AUTO) {
        /* Explicit encoding specified - use directly */
        cfg->detected_encoding = cfg->encoding;
        client->codec.detected_encoding = cfg->encoding;
        client->codec.encoding_detected = true;
    } else if (cfg->fire_and_forget) {
        /* No responses expected - default to binary */
        cfg->detected_encoding = ENCODING_BINARY;
        client->codec.detected_encoding = ENCODING_BINARY;
        client->codec.encoding_detected = true;
    } else {
        /* Must probe server to detect encoding */
        if (!probe_server_encoding(client)) {
            fprintf(stderr, "Failed to detect server encoding\n");
            transport_disconnect(&client->transport);
            return false;
        }
        if (!cfg->quiet) {
            printf("Server encoding: %s\n",
                   encoding_type_str(cfg->detected_encoding));
        }
    }

    client->connected = true;

    /* Join multicast if configured */
    if (cfg->multicast.enabled) {
        if (!engine_client_join_multicast(client, cfg->multicast.group,
                                          cfg->multicast.port)) {
            /* Non-fatal - continue without multicast */
            fprintf(stderr, "Warning: multicast join failed\n");
        }
    }

    return true;
}

void engine_client_disconnect(engine_client_t* client) {
    if (client == NULL) {
        return;
    }

    if (client->multicast_active) {
        engine_client_leave_multicast(client);
    }

    transport_disconnect(&client->transport);
    client->connected = false;
}

void engine_client_destroy(engine_client_t* client) {
    if (client == NULL) {
        return;
    }
    engine_client_disconnect(client);
}

bool engine_client_is_connected(const engine_client_t* client) {
    if (client == NULL) {
        return false;
    }
    return client->connected && transport_is_connected(&client->transport);
}

/* ============================================================
 * Multicast
 * ============================================================ */

bool engine_client_join_multicast(engine_client_t* client,
                                  const char* group, uint16_t port) {
    if (client == NULL || group == NULL) {
        return false;
    }

    if (client->multicast_active) {
        return true;
    }

    if (!client->config.quiet) {
        printf("Joining multicast group %s:%u...\n", group, port);
    }

    if (!multicast_receiver_join(&client->multicast, group, port)) {
        fprintf(stderr, "Failed to join multicast group\n");
        return false;
    }

    client->multicast_active = true;

    /* Copy group string safely */
    size_t group_len = strlen(group);
    size_t max_len = sizeof(client->config.multicast.group) - 1;
    if (group_len > max_len) {
        group_len = max_len;
    }
    memcpy(client->config.multicast.group, group, group_len);
    client->config.multicast.group[group_len] = '\0';
    client->config.multicast.port = port;
    client->config.multicast.enabled = true;

    return true;
}

void engine_client_leave_multicast(engine_client_t* client) {
    if (client == NULL) {
        return;
    }

    if (client->multicast_active) {
        multicast_receiver_leave(&client->multicast);
        client->multicast_active = false;
    }
}

/* ============================================================
 * Callbacks
 * ============================================================ */

void engine_client_set_response_callback(engine_client_t* client,
                                         response_callback_t callback,
                                         void* user_data) {
    if (client == NULL) {
        return;
    }
    client->response_callback = callback;
    client->response_user_data = user_data;
}

void engine_client_set_multicast_callback(engine_client_t* client,
                                          multicast_callback_t callback,
                                          void* user_data) {
    if (client == NULL) {
        return;
    }
    client->multicast_callback = callback;
    client->multicast_user_data = user_data;
}

/* ============================================================
 * Order Entry
 * ============================================================ */

uint32_t engine_client_send_order(engine_client_t* client,
                                  const char* symbol,
                                  uint32_t price,
                                  uint32_t quantity,
                                  side_t side,
                                  uint32_t order_id) {
    if (client == NULL || symbol == NULL || !client->connected) {
        return 0;
    }

    /* Auto-assign order ID if not provided */
    if (order_id == 0) {
        order_id = client->next_order_id++;
    } else if (order_id >= client->next_order_id) {
        client->next_order_id = order_id + 1;
    }

    const void* data = NULL;
    size_t len = 0;

    if (!codec_encode_new_order(&client->codec, client->config.user_id,
                                symbol, price, quantity, side, order_id,
                                &data, &len)) {
        return 0;
    }

    client->last_send_time = engine_client_now_ns();

    if (!transport_send(&client->transport, data, len)) {
        return 0;
    }

    client->orders_sent++;

    if (client->config.verbose) {
        printf("[SEND] %s %s %u@%u (order_id=%u)\n",
               side == SIDE_BUY ? "BUY" : "SELL",
               symbol, quantity, price, order_id);
    }

    return order_id;
}

bool engine_client_send_cancel(engine_client_t* client, uint32_t order_id) {
    if (client == NULL || !client->connected) {
        return false;
    }

    const void* data = NULL;
    size_t len = 0;

    if (!codec_encode_cancel(&client->codec, client->config.user_id,
                             order_id, &data, &len)) {
        return false;
    }

    client->last_send_time = engine_client_now_ns();

    if (!transport_send(&client->transport, data, len)) {
        return false;
    }

    client->cancels_sent++;

    if (client->config.verbose) {
        printf("[SEND] CANCEL order_id=%u\n", order_id);
    }

    return true;
}

bool engine_client_send_flush(engine_client_t* client) {
    if (client == NULL || !client->connected) {
        return false;
    }

    const void* data = NULL;
    size_t len = 0;

    if (!codec_encode_flush(&client->codec, &data, &len)) {
        return false;
    }

    client->last_send_time = engine_client_now_ns();

    if (!transport_send(&client->transport, data, len)) {
        return false;
    }

    client->flushes_sent++;

    if (client->config.verbose) {
        printf("[SEND] FLUSH\n");
    }

    return true;
}

/* ============================================================
 * Response Handling
 * ============================================================ */

static void update_latency_stats(engine_client_t* client) {
    if (client->last_send_time == 0) {
        return;
    }

    uint64_t now = engine_client_now_ns();
    uint64_t latency = now - client->last_send_time;

    client->total_latency += latency;
    client->latency_samples++;

    if (latency < client->min_latency) {
        client->min_latency = latency;
    }
    if (latency > client->max_latency) {
        client->max_latency = latency;
    }

    client->last_recv_time = now;
}

static void process_response(engine_client_t* client,
                             const output_msg_t* msg,
                             bool is_multicast) {
    if (!is_multicast) {
        update_latency_stats(client);
        client->responses_received++;
    } else {
        client->multicast_received++;
    }

    /* Invoke appropriate callback */
    if (is_multicast && client->multicast_callback != NULL) {
        client->multicast_callback(msg, client->multicast_user_data);
    } else if (!is_multicast && client->response_callback != NULL) {
        client->response_callback(msg, client->response_user_data);
    }
}

int engine_client_poll(engine_client_t* client) {
    if (client == NULL) {
        return 0;
    }

    char buffer[CLIENT_RECV_BUFFER_SIZE];
    size_t len = 0;
    output_msg_t msg;
    int count = 0;

    /* Poll TCP/UDP - bounded loop */
    if (client->connected && client->config.mode != MODE_MULTICAST_ONLY) {
        for (int i = 0; i < MAX_RECV_ATTEMPTS && transport_has_data(&client->transport); i++) {
            if (!transport_recv(&client->transport, buffer, sizeof(buffer), &len, 0)) {
                break;
            }
            if (codec_decode_response(&client->codec, buffer, len, &msg)) {
                process_response(client, &msg, false);
                count++;
            }
        }
    }

    /* Poll multicast - bounded loop */
    if (client->multicast_active) {
        struct pollfd pfd;
        pfd.fd = multicast_receiver_get_fd(&client->multicast);
        pfd.events = POLLIN;
        pfd.revents = 0;

        for (int i = 0; i < MAX_RECV_ATTEMPTS; i++) {
            if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
                break;
            }
            if (!multicast_receiver_recv(&client->multicast, buffer,
                                         sizeof(buffer), &len, 0)) {
                break;
            }
            if (codec_decode_response(&client->codec, buffer, len, &msg)) {
                process_response(client, &msg, true);
                count++;
            }
        }
    }

    return count;
}

bool engine_client_recv(engine_client_t* client,
                        output_msg_t* msg,
                        int timeout_ms) {
    if (client == NULL || msg == NULL) {
        return false;
    }

    if (!client->connected && !client->multicast_active) {
        return false;
    }

    char buffer[CLIENT_RECV_BUFFER_SIZE];
    size_t len = 0;

    /* Build poll set */
    struct pollfd pfds[MAX_POLL_FDS];
    int nfds = 0;

    if (client->connected && client->config.mode != MODE_MULTICAST_ONLY) {
        pfds[nfds].fd = transport_get_fd(&client->transport);
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }

    if (client->multicast_active) {
        pfds[nfds].fd = multicast_receiver_get_fd(&client->multicast);
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }

    if (nfds == 0) {
        return false;
    }

    int ret = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (ret <= 0) {
        return false;
    }

    /* Check each fd - bounded by MAX_POLL_FDS */
    for (int i = 0; i < nfds; i++) {
        if (!(pfds[i].revents & POLLIN)) {
            continue;
        }

        bool is_multicast = client->multicast_active &&
                            pfds[i].fd == multicast_receiver_get_fd(&client->multicast);

        if (is_multicast) {
            if (!multicast_receiver_recv(&client->multicast, buffer,
                                         sizeof(buffer), &len, 0)) {
                continue;
            }
        } else {
            if (!transport_recv(&client->transport, buffer,
                                sizeof(buffer), &len, 0)) {
                continue;
            }
        }

        if (codec_decode_response(&client->codec, buffer, len, msg)) {
            process_response(client, msg, is_multicast);
            return true;
        }
    }

    return false;
}

int engine_client_recv_all(engine_client_t* client, int timeout_ms) {
    if (client == NULL) {
        return 0;
    }

    output_msg_t msg;
    int count = 0;

    /* Bounded loop */
    for (int i = 0; i < MAX_RECV_ATTEMPTS; i++) {
        if (!engine_client_recv(client, &msg, timeout_ms)) {
            break;
        }
        count++;
        timeout_ms = 50;  /* Shorter timeout after first */
    }

    return count;
}

bool engine_client_wait_for(engine_client_t* client,
                            output_msg_type_t type,
                            output_msg_t* msg,
                            int timeout_ms) {
    if (client == NULL || msg == NULL || timeout_ms < 0) {
        return false;
    }

    uint64_t start = engine_client_now_ns();
    uint64_t deadline = start + (uint64_t)timeout_ms * 1000000ULL;

    /* Bounded by timeout */
    for (int i = 0; i < MAX_RECV_ATTEMPTS; i++) {
        uint64_t now = engine_client_now_ns();
        if (now >= deadline) {
            break;
        }

        int remaining = (int)((deadline - now) / 1000000ULL);
        if (remaining <= 0) {
            remaining = 1;
        }

        if (engine_client_recv(client, msg, remaining)) {
            if (msg->type == type) {
                return true;
            }
        }
    }

    return false;
}

/* ============================================================
 * Utilities
 * ============================================================ */

transport_type_t engine_client_get_transport(const engine_client_t* client) {
    if (client == NULL) {
        return TRANSPORT_AUTO;
    }
    return client->config.detected_transport;
}

encoding_type_t engine_client_get_encoding(const engine_client_t* client) {
    if (client == NULL) {
        return ENCODING_AUTO;
    }
    if (client->codec.encoding_detected) {
        return client->codec.detected_encoding;
    }
    return client->config.encoding;
}

uint32_t engine_client_peek_next_order_id(const engine_client_t* client) {
    if (client == NULL) {
        return 0;
    }
    return client->next_order_id;
}

void engine_client_reset_order_id(engine_client_t* client, uint32_t start_id) {
    if (client == NULL) {
        return;
    }
    client->next_order_id = start_id;
}

void engine_client_reset_stats(engine_client_t* client) {
    if (client == NULL) {
        return;
    }

    client->orders_sent = 0;
    client->cancels_sent = 0;
    client->flushes_sent = 0;
    client->responses_received = 0;
    client->multicast_received = 0;
    client->total_latency = 0;
    client->latency_samples = 0;
    client->min_latency = UINT64_MAX;
    client->max_latency = 0;
}

void engine_client_print_stats(const engine_client_t* client) {
    if (client == NULL) {
        return;
    }

    printf("\n=== Engine Client Statistics ===\n\n");

    printf("Connection:\n");
    printf("  Host:              %s:%u\n", client->config.host, client->config.port);
    printf("  Transport:         %s\n",
           transport_type_str(engine_client_get_transport(client)));
    printf("  Encoding:          %s\n",
           encoding_type_str(engine_client_get_encoding(client)));
    printf("  Connected:         %s\n", client->connected ? "yes" : "no");
    printf("\n");

    printf("Messages:\n");
    printf("  Orders sent:       %lu\n", (unsigned long)client->orders_sent);
    printf("  Cancels sent:      %lu\n", (unsigned long)client->cancels_sent);
    printf("  Flushes sent:      %lu\n", (unsigned long)client->flushes_sent);
    printf("  Responses recv:    %lu\n", (unsigned long)client->responses_received);
    if (client->multicast_active) {
        printf("  Multicast recv:    %lu\n", (unsigned long)client->multicast_received);
    }
    printf("\n");

    if (client->latency_samples > 0) {
        uint64_t avg_ns = client->total_latency / client->latency_samples;
        printf("Latency (round-trip):\n");
        printf("  Samples:           %lu\n", (unsigned long)client->latency_samples);
        printf("  Min:               %lu ns (%.3f us)\n",
               (unsigned long)client->min_latency,
               (double)client->min_latency / 1000.0);
        printf("  Avg:               %lu ns (%.3f us)\n",
               (unsigned long)avg_ns, (double)avg_ns / 1000.0);
        printf("  Max:               %lu ns (%.3f us)\n",
               (unsigned long)client->max_latency,
               (double)client->max_latency / 1000.0);
        printf("\n");
    }

    transport_print_stats(&client->transport);

    if (client->multicast_active) {
        printf("\n");
        multicast_receiver_print_stats(&client->multicast);
    }

    printf("\n");
    codec_print_stats(&client->codec);
}

uint64_t engine_client_get_avg_latency_ns(const engine_client_t* client) {
    if (client == NULL || client->latency_samples == 0) {
        return 0;
    }
    return client->total_latency / client->latency_samples;
}

uint64_t engine_client_get_min_latency_ns(const engine_client_t* client) {
    if (client == NULL || client->min_latency == UINT64_MAX) {
        return 0;
    }
    return client->min_latency;
}

uint64_t engine_client_get_max_latency_ns(const engine_client_t* client) {
    if (client == NULL) {
        return 0;
    }
    return client->max_latency;
}

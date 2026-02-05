#include "unified_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <errno.h>

/* ============================================================================
 * DEBUG: Timing helper
 * ============================================================================ */
static inline uint64_t debug_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* DEBUG: Aggregate stats for the router thread */
static uint64_t dbg_sends_total = 0;
static uint64_t dbg_sends_slow = 0;          /* send() took > 1ms */
static uint64_t dbg_sends_blocked = 0;       /* send() returned EAGAIN or partial */
static uint64_t dbg_send_time_total_ns = 0;  /* cumulative time in send() */
static uint64_t dbg_tob_broadcasts_total = 0;
static uint64_t dbg_tob_time_total_ns = 0;
static uint64_t dbg_envelopes_total = 0;
static uint64_t dbg_envelope_time_total_ns = 0;
static uint64_t dbg_envelopes_slow = 0;      /* envelope took > 1ms */

/* ============================================================================
 * Helper: Send with length-prefix framing for TCP binary clients
 * ============================================================================ */
static ssize_t tcp_send_with_framing(int fd, const void* data, size_t len,
                                     bool use_length_prefix) {
    if (fd < 0 || data == NULL || len == 0) {
        return -1;
    }

    uint64_t t0 = debug_now_ns();

    for (;;) {
        if (use_length_prefix) {
            /* Send 4-byte big-endian length prefix + data atomically */
            uint32_t net_len = htonl((uint32_t)len);
            struct iovec iov[2];
            iov[0].iov_base = &net_len;
            iov[0].iov_len = 4;
            iov[1].iov_base = (void*)data;
            iov[1].iov_len = len;
            ssize_t sent = writev(fd, iov, 2);
            if (sent < 0 && errno == EINTR) {
                continue;
            }

            uint64_t elapsed_ns = debug_now_ns() - t0;
            dbg_sends_total++;
            dbg_send_time_total_ns += elapsed_ns;
            if (elapsed_ns > 1000000ULL) {  /* > 1ms */
                dbg_sends_slow++;
                fprintf(stderr, "[DBG-SEND] SLOW writev fd=%d len=%zu took %.2f ms (total_slow=%llu)\n",
                        fd, len, (double)elapsed_ns / 1e6,
                        (unsigned long long)dbg_sends_slow);
            }
            if (sent != (ssize_t)(4 + len)) {
                dbg_sends_blocked++;
                fprintf(stderr, "[DBG-SEND] PARTIAL/FAIL writev fd=%d expected=%zu got=%zd errno=%s\n",
                        fd, 4 + len, sent, strerror(errno));
            }

            /* Return data length for consistency checking */
            return (sent == (ssize_t)(4 + len)) ? (ssize_t)len : -1;
        } else {
            ssize_t sent = send(fd, data, len, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR) {
                continue;
            }

            uint64_t elapsed_ns = debug_now_ns() - t0;
            dbg_sends_total++;
            dbg_send_time_total_ns += elapsed_ns;
            if (elapsed_ns > 1000000ULL) {  /* > 1ms */
                dbg_sends_slow++;
                fprintf(stderr, "[DBG-SEND] SLOW send fd=%d len=%zu took %.2f ms (total_slow=%llu)\n",
                        fd, len, (double)elapsed_ns / 1e6,
                        (unsigned long long)dbg_sends_slow);
            }
            if (sent < 0) {
                dbg_sends_blocked++;
                fprintf(stderr, "[DBG-SEND] FAIL send fd=%d len=%zu errno=%s\n",
                        fd, len, strerror(errno));
            } else if ((size_t)sent < len) {
                dbg_sends_blocked++;
                fprintf(stderr, "[DBG-SEND] PARTIAL send fd=%d expected=%zu got=%zd\n",
                        fd, len, sent);
            }

            return sent;
        }
    }
}

/* ============================================================================
 * Send to Multicast
 * ============================================================================ */
void unified_send_multicast(unified_server_t* server, const output_msg_t* msg) {
    if (server->multicast_fd < 0) {
        return;
    }

    size_t bin_len = 0;
    const void* bin_data = binary_message_formatter_format(&server->bin_formatter, msg, &bin_len);
    if (bin_data && bin_len > 0) {
        ssize_t sent = sendto(server->multicast_fd, bin_data, bin_len, 0,
                              (struct sockaddr*)&server->multicast_addr,
                              sizeof(server->multicast_addr));
        if (sent < 0) {
            fprintf(stderr, "[Multicast] ERROR: %s\n", strerror(errno));
        }
        atomic_fetch_add(&server->multicast_messages, 1);
    } else {
        fprintf(stderr, "[Multicast] SKIP - no data formatted\n");
    }
}

/* ============================================================================
 * Send to Client Implementation
 * ============================================================================ */
bool unified_send_to_client(unified_server_t* server,
                            uint32_t client_id,
                            const output_msg_t* msg) {
    if (client_id == 0) return false;

    client_entry_t entry;
    if (!client_registry_find(server->registry, client_id, &entry)) {
        if (!server->config.quiet_mode) {
            fprintf(stderr, "[Router] Client %u not found in registry\n", client_id);
        }
        return false;
    }

    /* Snapshot says inactive? treat as not found */
    if (!entry.active) {
        return false;
    }

    /* Format message based on client protocol */
    const void* data = NULL;
    size_t len = 0;

    if (entry.protocol == CLIENT_PROTOCOL_BINARY) {
        data = binary_message_formatter_format(&server->bin_formatter, msg, &len);
    } else {
        const char* line = message_formatter_format(&server->csv_formatter, msg);
        if (line) {
            data = line;
            len = strlen(line);
        } else {
            fprintf(stderr, "[Router] Failed to format CSV message for client %u\n", client_id);
            return false;
        }
    }

    if (!data || len == 0) {
        fprintf(stderr, "[Router] No data to send to client %u\n", client_id);
        return false;
    }

    bool success = false;

    if (entry.transport == TRANSPORT_TCP) {
        /* TCP send - use length-prefix framing for binary protocol clients */
        int fd = entry.handle.tcp_fd;
        if (fd >= 0) {
            bool use_length_prefix = (entry.protocol == CLIENT_PROTOCOL_BINARY);
            ssize_t sent = tcp_send_with_framing(fd, data, len, use_length_prefix);
            success = (sent == (ssize_t)len);
            if (!success && !server->config.quiet_mode) {
                fprintf(stderr, "[Router] TCP send to client %u failed (%s)\n",
                        client_id, strerror(errno));
            }
        } else {
            if (!server->config.quiet_mode) {
                fprintf(stderr, "[Router] Invalid TCP fd for client %u\n", client_id);
            }
        }
    } else if (entry.transport == TRANSPORT_UDP) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = entry.handle.udp_addr.addr;
        addr.sin_port = entry.handle.udp_addr.port;
        ssize_t sent = sendto(server->udp_fd, data, len, 0,
                              (struct sockaddr*)&addr, sizeof(addr));
        success = (sent == (ssize_t)len);
    } else {
        /* Unknown transport */
        success = false;
    }

    if (success) {
        client_registry_inc_sent(server->registry, client_id);
    }

    return success;
}

/* ============================================================================
 * Broadcast to All Clients
 * ============================================================================ */
void unified_broadcast_to_all(unified_server_t* server, const output_msg_t* msg) {
    uint64_t t0 = debug_now_ns();

    /* Pre-format for both protocols */
    const char* csv_line = message_formatter_format(&server->csv_formatter, msg);
    size_t csv_len = csv_line ? strlen(csv_line) : 0;

    size_t bin_len = 0;
    const void* bin_data = binary_message_formatter_format(&server->bin_formatter, msg, &bin_len);

    /* Get all client IDs */
    uint32_t client_ids[MAX_REGISTERED_CLIENTS];
    uint32_t count = client_registry_get_all_ids(server->registry, client_ids, MAX_REGISTERED_CLIENTS);

    for (uint32_t i = 0; i < count; i++) {
        client_entry_t entry;
        if (!client_registry_find(server->registry, client_ids[i], &entry)) {
            continue;
        }
        if (!entry.active) continue;

        const void* data = NULL;
        size_t len = 0;
        bool is_binary = (entry.protocol == CLIENT_PROTOCOL_BINARY);

        if (is_binary) {
            data = bin_data;
            len = bin_len;
        } else {
            data = csv_line;
            len = csv_len;
        }

        if (!data || len == 0) continue;

        if (entry.transport == TRANSPORT_TCP) {
            int fd = entry.handle.tcp_fd;
            if (fd >= 0) {
                /* Use length-prefix framing for binary protocol TCP clients */
                tcp_send_with_framing(fd, data, len, is_binary);
            }
        } else if (entry.transport == TRANSPORT_UDP) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = entry.handle.udp_addr.addr;
            addr.sin_port = entry.handle.udp_addr.port;
            sendto(server->udp_fd, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
        }

        client_registry_inc_sent(server->registry, client_ids[i]);
    }

    uint64_t elapsed_ns = debug_now_ns() - t0;
    dbg_tob_broadcasts_total++;
    dbg_tob_time_total_ns += elapsed_ns;

    if (elapsed_ns > 1000000ULL) {  /* > 1ms */
        fprintf(stderr, "[DBG-TOB] SLOW broadcast to %u clients took %.2f ms\n",
                count, (double)elapsed_ns / 1e6);
    }

    atomic_fetch_add(&server->tob_broadcasts, 1);
}

/* ============================================================================
 * Process Single Output Envelope
 * ============================================================================ */
static void process_output_envelope(unified_server_t* server,
                                   const output_msg_envelope_t* envelope) {
    uint64_t t0 = debug_now_ns();

    const output_msg_t* msg = &envelope->msg;
    uint32_t originator = envelope->client_id;

    /* Route based on message type */
    switch (msg->type) {
        case OUTPUT_MSG_ACK:
        case OUTPUT_MSG_CANCEL_ACK:
            /* Send to originator only */
            unified_send_to_client(server, originator, msg);
            break;

        case OUTPUT_MSG_TRADE: {
            /* Send to both buyer and seller */
            uint32_t buyer_client = user_client_map_get(server->user_map,
                                                        msg->data.trade.user_id_buy);
            uint32_t seller_client = user_client_map_get(server->user_map,
                                                         msg->data.trade.user_id_sell);
            if (buyer_client != 0) {
                unified_send_to_client(server, buyer_client, msg);
            }
            if (seller_client != 0 && seller_client != buyer_client) {
                unified_send_to_client(server, seller_client, msg);
            }
            break;
        }

        case OUTPUT_MSG_TOP_OF_BOOK:
            /* Broadcast to all connected clients */
            unified_broadcast_to_all(server, msg);
            break;

        default:
            break;
    }

    /* Send to multicast (always binary) */
    unified_send_multicast(server, msg);

    uint64_t elapsed_ns = debug_now_ns() - t0;
    dbg_envelopes_total++;
    dbg_envelope_time_total_ns += elapsed_ns;

    if (elapsed_ns > 1000000ULL) {  /* > 1ms */
        dbg_envelopes_slow++;
        const char* type_str = "UNKNOWN";
        switch (msg->type) {
            case OUTPUT_MSG_ACK:        type_str = "ACK"; break;
            case OUTPUT_MSG_TRADE:      type_str = "TRADE"; break;
            case OUTPUT_MSG_CANCEL_ACK: type_str = "CANCEL"; break;
            case OUTPUT_MSG_TOP_OF_BOOK: type_str = "TOB"; break;
            default: break;
        }
        fprintf(stderr, "[DBG-ENV] SLOW envelope #%llu type=%s took %.2f ms (slow_total=%llu)\n",
                (unsigned long long)dbg_envelopes_total, type_str,
                (double)elapsed_ns / 1e6,
                (unsigned long long)dbg_envelopes_slow);
    }

    atomic_fetch_add(&server->messages_routed, 1);
}

/* ============================================================================
 * Output Router Thread
 * ============================================================================ */
void* unified_output_router_thread(void* arg) {
    unified_server_t* server = (unified_server_t*)arg;
    output_msg_envelope_t envelope;

    /* Progress tracking for quiet mode */
    uint64_t last_progress_time = unified_get_timestamp_ns();
    uint64_t start_time = last_progress_time;
    uint64_t last_routed = 0;
    const uint64_t PROGRESS_INTERVAL_NS = 10ULL * 1000000000ULL;  /* 10 seconds */

    /* DEBUG: More frequent debug stats - every 2 seconds */
    uint64_t last_debug_time = last_progress_time;
    uint64_t last_dbg_envelopes = 0;
    uint64_t last_dbg_sends = 0;
    uint64_t last_dbg_send_time = 0;
    uint64_t idle_spins = 0;
    const uint64_t DEBUG_INTERVAL_NS = 2ULL * 1000000000ULL;

    while (!atomic_load(&g_shutdown)) {
        bool got_message = false;

        /* Drain output queue 0 */
        if (output_envelope_queue_dequeue(server->output_queue_0, &envelope)) {
            process_output_envelope(server, &envelope);
            got_message = true;
        }

        /* Drain output queue 1 (if dual processor) */
        if (server->output_queue_1 &&
            output_envelope_queue_dequeue(server->output_queue_1, &envelope)) {
            process_output_envelope(server, &envelope);
            got_message = true;
        }

        if (!got_message) {
            idle_spins++;
            struct timespec ts = {0, 1000};  /* 1µs */
            nanosleep(&ts, NULL);
        }

        /* DEBUG: Periodic stats every 2 seconds */
        uint64_t now = debug_now_ns();
        if (now - last_debug_time >= DEBUG_INTERVAL_NS) {
            double interval_sec = (double)(now - last_debug_time) / 1e9;
            uint64_t env_delta = dbg_envelopes_total - last_dbg_envelopes;
            uint64_t send_delta = dbg_sends_total - last_dbg_sends;
            uint64_t send_time_delta = dbg_send_time_total_ns - last_dbg_send_time;
            double env_rate = interval_sec > 0 ? env_delta / interval_sec : 0;
            double send_rate = interval_sec > 0 ? send_delta / interval_sec : 0;
            double avg_send_us = send_delta > 0 ? (double)send_time_delta / send_delta / 1000.0 : 0;
            double pct_in_send = interval_sec > 0 ? (double)send_time_delta / (interval_sec * 1e9) * 100.0 : 0;

            fprintf(stderr,
                    "[DBG-ROUTER] %.1fs | env: %llu (%.0f/s) | sends: %llu (%.0f/s) "
                    "| avg_send: %.1f us | in_send: %.1f%% | slow_send: %llu | blocked: %llu "
                    "| slow_env: %llu | idle: %llu\n",
                    (double)(now - start_time) / 1e9,
                    (unsigned long long)dbg_envelopes_total, env_rate,
                    (unsigned long long)dbg_sends_total, send_rate,
                    avg_send_us, pct_in_send,
                    (unsigned long long)dbg_sends_slow,
                    (unsigned long long)dbg_sends_blocked,
                    (unsigned long long)dbg_envelopes_slow,
                    (unsigned long long)idle_spins);

            last_debug_time = now;
            last_dbg_envelopes = dbg_envelopes_total;
            last_dbg_sends = dbg_sends_total;
            last_dbg_send_time = dbg_send_time_total_ns;
            idle_spins = 0;
        }

        /* Progress update in quiet mode */
        if (server->config.quiet_mode) {
            if (now - last_progress_time >= PROGRESS_INTERVAL_NS) {
                uint64_t total_routed = atomic_load(&server->messages_routed);
                uint64_t elapsed_ns = now - start_time;
                double elapsed_sec = (double)elapsed_ns / 1e9;
                uint64_t interval_msgs = total_routed - last_routed;
                double interval_sec = (double)(now - last_progress_time) / 1e9;
                double current_rate = interval_sec > 0 ? interval_msgs / interval_sec : 0;
                double avg_rate = elapsed_sec > 0 ? total_routed / elapsed_sec : 0;

                client_registry_stats_t stats;
                client_registry_get_stats(server->registry, &stats);

                fprintf(stderr,
                        "[PROGRESS] %6.1fs | %12llu routed | %8.2fK msg/s (avg: %.2fK) | TCP: %u UDP: %u\n",
                        elapsed_sec,
                        (unsigned long long)total_routed,
                        current_rate / 1000.0,
                        avg_rate / 1000.0,
                        stats.tcp_clients_active,
                        stats.udp_clients_active);

                last_progress_time = now;
                last_routed = total_routed;
            }
        }
    }

    /* DEBUG: Final summary */
    fprintf(stderr, "\n[DBG-ROUTER] === FINAL DEBUG SUMMARY ===\n");
    fprintf(stderr, "[DBG-ROUTER] Total envelopes:     %llu\n", (unsigned long long)dbg_envelopes_total);
    fprintf(stderr, "[DBG-ROUTER] Total sends:         %llu\n", (unsigned long long)dbg_sends_total);
    fprintf(stderr, "[DBG-ROUTER] Slow sends (>1ms):   %llu\n", (unsigned long long)dbg_sends_slow);
    fprintf(stderr, "[DBG-ROUTER] Blocked sends:       %llu\n", (unsigned long long)dbg_sends_blocked);
    fprintf(stderr, "[DBG-ROUTER] Slow envelopes:      %llu\n", (unsigned long long)dbg_envelopes_slow);
    fprintf(stderr, "[DBG-ROUTER] TOB broadcasts:      %llu\n", (unsigned long long)dbg_tob_broadcasts_total);
    if (dbg_sends_total > 0) {
        fprintf(stderr, "[DBG-ROUTER] Avg send time:      %.1f us\n",
                (double)dbg_send_time_total_ns / dbg_sends_total / 1000.0);
    }
    if (dbg_tob_broadcasts_total > 0) {
        fprintf(stderr, "[DBG-ROUTER] Avg TOB time:       %.1f us\n",
                (double)dbg_tob_time_total_ns / dbg_tob_broadcasts_total / 1000.0);
    }
    if (dbg_envelopes_total > 0) {
        fprintf(stderr, "[DBG-ROUTER] Avg envelope time:  %.1f us\n",
                (double)dbg_envelope_time_total_ns / dbg_envelopes_total / 1000.0);
    }
    fprintf(stderr, "[DBG-ROUTER] ========================\n\n");

    fprintf(stderr, "[Router] Output router stopped\n");
    return NULL;
}

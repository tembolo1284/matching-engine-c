#include "network/tcp_listener.h"
#include "protocol/csv/message_parser.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/binary/binary_message_formatter.h"
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#define MAX_EPOLL_EVENTS 128
#define EPOLL_TIMEOUT_MS 100

static message_parser_t g_csv_parser;
static binary_message_parser_t g_binary_parser;
static message_formatter_t g_csv_formatter;
static binary_message_formatter_t g_binary_formatter;

// Forward declarations
static bool setup_listening_socket(tcp_listener_context_t* ctx);
static bool setup_epoll(tcp_listener_context_t* ctx);
static void handle_new_connection(tcp_listener_context_t* ctx);
static void handle_client_read(tcp_listener_context_t* ctx, uint32_t client_id);
static void handle_client_write(tcp_listener_context_t* ctx, uint32_t client_id);
static void disconnect_client(tcp_listener_context_t* ctx, uint32_t client_id);
static bool set_nonblocking(int fd);
static void process_output_queues(tcp_listener_context_t* ctx);

bool tcp_listener_init(tcp_listener_context_t* ctx,
                       const tcp_listener_config_t* config,
                       tcp_client_registry_t* client_registry,
                       input_envelope_queue_t* input_queue,
                       atomic_bool* shutdown_flag) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->config = *config;
    ctx->client_registry = client_registry;
    ctx->input_queue = input_queue;
    ctx->shutdown_flag = shutdown_flag;
    ctx->listen_fd = -1;
    ctx->epoll_fd = -1;

    message_parser_init(&g_csv_parser);
    binary_message_parser_init(&g_binary_parser);
    message_formatter_init(&g_csv_formatter);
    binary_message_formatter_init(&g_binary_formatter);

    return true;
}

void tcp_listener_cleanup(tcp_listener_context_t* ctx) {
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
        ctx->epoll_fd = -1;
    }

    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
}

void* tcp_listener_thread(void* arg) {
    tcp_listener_context_t* ctx = (tcp_listener_context_t*)arg;

    fprintf(stderr, "[TCP Listener] Starting on port %u\n", ctx->config.port);

    // Setup listening socket
    if (!setup_listening_socket(ctx)) {
        fprintf(stderr, "[TCP Listener] Failed to setup listening socket\n");
        return NULL;
    }

    // Setup epoll
    if (!setup_epoll(ctx)) {
        fprintf(stderr, "[TCP Listener] Failed to setup epoll\n");
        close(ctx->listen_fd);
        return NULL;
    }

    fprintf(stderr, "[TCP Listener] Ready and listening\n");

    // Main event loop
    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (!atomic_load(ctx->shutdown_flag)) {
        // Check output queues for pending messages
        process_output_queues(ctx);

        int nfds = epoll_wait(ctx->epoll_fd, events, MAX_EPOLL_EVENTS, EPOLL_TIMEOUT_MS);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted by signal
            }
            fprintf(stderr, "[TCP Listener] epoll_wait error: %s\n", strerror(errno));
            break;
        }

        // Process events
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t event_mask = events[i].events;

            // Check for errors
            if (event_mask & (EPOLLERR | EPOLLHUP)) {
                if (fd == ctx->listen_fd) {
                    fprintf(stderr, "[TCP Listener] Error on listening socket\n");
                    goto cleanup;
                } else {
                    // Client error - disconnect
                    // Need to find client_id from fd
                    for (size_t j = 0; j < MAX_TCP_CLIENTS; j++) {
                        tcp_client_t* client = &ctx->client_registry->clients[j];
                        if (client->active && client->socket_fd == fd) {
                            disconnect_client(ctx, client->client_id);
                            break;
                        }
                    }
                    continue;
                }
            }

            // New connection
            if (fd == ctx->listen_fd) {
                handle_new_connection(ctx);
                continue;
            }

            // Client I/O - find client by fd
            tcp_client_t* client = NULL;
            for (size_t j = 0; j < MAX_TCP_CLIENTS; j++) {
                if (ctx->client_registry->clients[j].active &&
                    ctx->client_registry->clients[j].socket_fd == fd) {
                    client = &ctx->client_registry->clients[j];
                    break;
                }
            }

            if (!client) {
                continue;  // Client not found (shouldn't happen)
            }

            if (event_mask & EPOLLIN) {
                handle_client_read(ctx, client->client_id);
            }

            if (event_mask & EPOLLOUT) {
                handle_client_write(ctx, client->client_id);
            }
        }
    }

cleanup:
    fprintf(stderr, "[TCP Listener] Shutting down\n");
    tcp_listener_print_stats(ctx);
    tcp_listener_cleanup(ctx);
    return NULL;
}

static bool setup_listening_socket(tcp_listener_context_t* ctx) {
    // Create socket
    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
        fprintf(stderr, "[TCP Listener] socket() failed: %s\n", strerror(errno));
        return false;
    }

    // Set socket options
    int optval = 1;
    if (setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0) {
        fprintf(stderr, "[TCP Listener] setsockopt(SO_REUSEADDR) failed: %s\n",
                strerror(errno));
        close(ctx->listen_fd);
        return false;
    }

    // Make non-blocking
    if (!set_nonblocking(ctx->listen_fd)) {
        close(ctx->listen_fd);
        return false;
    }

    // Bind to port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(ctx->config.port);

    if (bind(ctx->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[TCP Listener] bind() failed: %s\n", strerror(errno));
        close(ctx->listen_fd);
        return false;
    }

    // Listen
    int backlog = ctx->config.listen_backlog > 0 ? ctx->config.listen_backlog : 10;
    if (listen(ctx->listen_fd, backlog) < 0) {
        fprintf(stderr, "[TCP Listener] listen() failed: %s\n", strerror(errno));
        close(ctx->listen_fd);
        return false;
    }

    return true;
}

static bool setup_epoll(tcp_listener_context_t* ctx) {
    ctx->epoll_fd = epoll_create1(0);
    if (ctx->epoll_fd < 0) {
        fprintf(stderr, "[TCP Listener] epoll_create1() failed: %s\n", strerror(errno));
        return false;
    }

    // Add listening socket to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = ctx->listen_fd;

    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev) < 0) {
        fprintf(stderr, "[TCP Listener] epoll_ctl(listen) failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}

static void handle_new_connection(tcp_listener_context_t* ctx) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(ctx->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[TCP Listener] accept() failed: %s\n", strerror(errno));
        }
        return;
    }

    // Make client socket non-blocking
    if (!set_nonblocking(client_fd)) {
        close(client_fd);
        return;
    }

    // Add client to registry
    uint32_t client_id;
    if (!tcp_client_add(ctx->client_registry, client_fd, client_addr, &client_id)) {
        fprintf(stderr, "[TCP Listener] Client registry full, rejecting connection\n");
        close(client_fd);
        return;
    }

    // Add client socket to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  // Edge-triggered for efficiency
    ev.data.fd = client_fd;

    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
        fprintf(stderr, "[TCP Listener] epoll_ctl(client) failed: %s\n", strerror(errno));
        tcp_client_remove(ctx->client_registry, client_id);
        return;
    }

    ctx->total_connections++;
}

static void handle_client_read(tcp_listener_context_t* ctx, uint32_t client_id) {
    tcp_client_t* client = tcp_client_get(ctx->client_registry, client_id);
    if (!client) {
        return;
    }

    // Read from socket
    char buffer[4096];
    ssize_t n = read(client->socket_fd, buffer, sizeof(buffer));

    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[TCP Listener] read() error for client %u: %s\n",
                    client_id, strerror(errno));
            disconnect_client(ctx, client_id);
        }
        return;
    }

    if (n == 0) {
        // Client closed connection
        disconnect_client(ctx, client_id);
        return;
    }

    client->bytes_received += n;
    ctx->total_bytes_received += n;

    // Process incoming data through framing layer
    const char* msg_data;
    size_t msg_len;

    if (framing_read_process(&client->read_state, buffer, n, &msg_data, &msg_len)) {
        // Complete message received - parse it
        input_msg_t input_msg;
        bool parsed = false;

        // Auto-detect CSV vs binary (binary starts with 0x4D magic byte)
        if (msg_len > 0 && (unsigned char)msg_data[0] == 0x4D) {
            // Binary protocol
            parsed = binary_message_parser_parse(&g_binary_parser, msg_data, msg_len, &input_msg);
        } else {
            // CSV protocol - needs null-terminated string
            char csv_buffer[4096];
            if (msg_len < sizeof(csv_buffer)) {
                memcpy(csv_buffer, msg_data, msg_len);
                csv_buffer[msg_len] = '\0';
                parsed = message_parser_parse(&g_csv_parser, csv_buffer, &input_msg);
            } else {
                parsed = false; // Message too long
            }
        }

        if (parsed) {
            // Validate userId matches client_id (security check)
            bool valid = true;
            switch (input_msg.type) {
                case INPUT_MSG_NEW_ORDER:
                    if (input_msg.data.new_order.user_id != client_id) {
                        fprintf(stderr, "[TCP Listener] Client %u tried to spoof userId %u\n",
                                client_id, input_msg.data.new_order.user_id);
                        valid = false;
                    }
                    break;
                case INPUT_MSG_CANCEL:
                    if (input_msg.data.cancel.user_id != client_id) {
                        fprintf(stderr, "[TCP Listener] Client %u tried to spoof userId %u\n",
                                client_id, input_msg.data.cancel.user_id);
                        valid = false;
                    }
                    break;
                case INPUT_MSG_FLUSH:
                    // Flush doesn't have userId - always valid
                    break;
            }

            if (valid) {
                // Create envelope and enqueue
                input_msg_envelope_t envelope = create_input_envelope(
                    &input_msg, client_id, client->messages_received
                );

                if (input_envelope_queue_enqueue(ctx->input_queue, &envelope)) {
                    client->messages_received++;
                    ctx->total_messages_received++;
                } else {
                    fprintf(stderr, "[TCP Listener] Input queue full, dropping message from client %u\n",
                            client_id);
                }
            }
        } else {
            fprintf(stderr, "[TCP Listener] Failed to parse message from client %u\n", client_id);
        }

        // Reset framing state for next message
        framing_read_state_init(&client->read_state);
    }
}

static void handle_client_write(tcp_listener_context_t* ctx, uint32_t client_id) {
    tcp_client_t* client = tcp_client_get(ctx->client_registry, client_id);
    if (!client) {
        return;
    }

    // If we have a pending write, continue it
    if (client->has_pending_write) {
        const char* data;
        size_t len;
        framing_write_get_remaining(&client->write_state, &data, &len);

        ssize_t n = write(client->socket_fd, data, len);

        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "[TCP Listener] write() error for client %u: %s\n",
                        client_id, strerror(errno));
                disconnect_client(ctx, client_id);
            }
            return;
        }

        framing_write_mark_written(&client->write_state, n);
        client->bytes_sent += n;
        ctx->total_bytes_sent += n;

        if (framing_write_is_complete(&client->write_state)) {
            client->has_pending_write = false;
            client->messages_sent++;
            ctx->total_messages_sent++;

            // Modify epoll to stop monitoring EPOLLOUT (save CPU)
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = client->socket_fd;
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, client->socket_fd, &ev);
        }
    }
}

static void process_output_queues(tcp_listener_context_t* ctx) {
    // Check each client's output queue and initiate writes
    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
        tcp_client_t* client = &ctx->client_registry->clients[i];

        if (!client->active || client->has_pending_write) {
            continue;
        }

        // Try to dequeue a message
        output_msg_t msg;
        if (!tcp_client_dequeue_output(client, &msg)) {
            continue;  // No messages
        }

        // Format message (CSV or binary) - FIXED
        char formatted[2048];
        size_t formatted_len;

        if (ctx->config.use_binary_output) {
            size_t len;
            const void* binary_data = binary_message_formatter_format(&g_binary_formatter, &msg, &len);
            if (binary_data && len <= sizeof(formatted)) {
                memcpy(formatted, binary_data, len);
                formatted_len = len;
            } else {
                formatted_len = 0;
            }
        } else {
            const char* csv_str = message_formatter_format(&g_csv_formatter, &msg);
            if (csv_str) {
                formatted_len = strlen(csv_str);
                if (formatted_len < sizeof(formatted)) {
                    memcpy(formatted, csv_str, formatted_len);
                } else {
                    formatted_len = 0;
                }
            } else {
                formatted_len = 0;
            }
        }

        if (formatted_len == 0) {
            fprintf(stderr, "[TCP Listener] Failed to format message for client %u\n",
                    client->client_id);
            continue;
        }

        // Initialize write state with framed message
        if (!framing_write_state_init(&client->write_state, formatted, formatted_len)) {
            fprintf(stderr, "[TCP Listener] Message too large for client %u\n",
                    client->client_id);
            continue;
        }

        client->has_pending_write = true;

        // Try immediate write (non-blocking)
        const char* data;
        size_t len;
        framing_write_get_remaining(&client->write_state, &data, &len);

        ssize_t n = write(client->socket_fd, data, len);

        if (n > 0) {
            framing_write_mark_written(&client->write_state, n);
            client->bytes_sent += n;
            ctx->total_bytes_sent += n;

            if (framing_write_is_complete(&client->write_state)) {
                client->has_pending_write = false;
                client->messages_sent++;
                ctx->total_messages_sent++;
            } else {
                // Partial write - add EPOLLOUT to be notified when socket is writable
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                ev.data.fd = client->socket_fd;
                epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, client->socket_fd, &ev);
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // Write error
            fprintf(stderr, "[TCP Listener] write() error for client %u: %s\n",
                    client->client_id, strerror(errno));
            disconnect_client(ctx, client->client_id);
        } else {
            // Would block - add EPOLLOUT
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
            ev.data.fd = client->socket_fd;
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, client->socket_fd, &ev);
        }
    }
}

static void disconnect_client(tcp_listener_context_t* ctx, uint32_t client_id) {
    tcp_client_t* client = tcp_client_get(ctx->client_registry, client_id);
    if (!client) {
        return;
    }

    // Remove from epoll
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, client->socket_fd, NULL);

    // Remove from registry (closes socket)
    tcp_client_remove(ctx->client_registry, client_id);

    // TODO: Notify processor to cancel all orders for this client
    // This will be handled by the output_router thread
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        fprintf(stderr, "[TCP Listener] fcntl(F_GETFL) failed: %s\n", strerror(errno));
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "[TCP Listener] fcntl(F_SETFL) failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}

void tcp_listener_print_stats(const tcp_listener_context_t* ctx) {
    fprintf(stderr, "\n=== TCP Listener Statistics ===\n");
    fprintf(stderr, "Total connections:     %lu\n", ctx->total_connections);
    fprintf(stderr, "Active clients:        %zu\n",
            tcp_client_get_active_count(ctx->client_registry));
    fprintf(stderr, "Messages received:     %lu\n", ctx->total_messages_received);
    fprintf(stderr, "Messages sent:         %lu\n", ctx->total_messages_sent);
    fprintf(stderr, "Bytes received:        %lu\n", ctx->total_bytes_received);
    fprintf(stderr, "Bytes sent:            %lu\n", ctx->total_bytes_sent);
}

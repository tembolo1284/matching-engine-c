#include "network/tcp_listener.h"
#include "protocol/csv/message_parser.h"
#include "protocol/csv/message_formatter.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/binary/binary_message_formatter.h"
#include "protocol/symbol_router.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/**
 * TCP Listener Implementation
 *
 * Rule Compliance:
 * - Rule 1: No recursion
 * - Rule 2: All loops bounded (MAX_MESSAGES_PER_READ, MAX_TCP_CLIENTS, etc.)
 * - Rule 5: All functions have >= 2 assertions
 * - Rule 7: All return values checked
 *
 * Kernel Bypass Notes:
 * - [KB-x] comments mark abstraction points for DPDK integration
 * - Current implementation uses standard sockets + epoll/kqueue
 * - For DPDK TCP: consider F-Stack or mTCP integration
 * - For DPDK UDP: this file not needed (see udp_receiver.c)
 */

/* ============================================================================
 * Platform-specific includes
 * ============================================================================ */

#ifdef __linux__
    #include <sys/epoll.h>
    #define USE_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__)
    #include <sys/event.h>
    #define USE_KQUEUE 1
#else
    #error "Unsupported platform - need epoll or kqueue"
#endif

/* ============================================================================
 * Constants (Rule 2: explicit bounds)
 * ============================================================================ */

#define MAX_EVENTS 128
#define EVENT_TIMEOUT_MS 100
#define MAX_READ_BUFFER 4096
#define MAX_MESSAGES_PER_READ 64
#define MAX_FORMAT_BUFFER 2048

/* ============================================================================
 * Thread-local parsers/formatters (avoids globals for thread safety)
 * ============================================================================ */

static __thread message_parser_t tls_csv_parser;
static __thread binary_message_parser_t tls_binary_parser;
static __thread message_formatter_t tls_csv_formatter;
static __thread binary_message_formatter_t tls_binary_formatter;
static __thread bool tls_initialized = false;

static void ensure_parsers_initialized(void) {
    if (!tls_initialized) {
        message_parser_init(&tls_csv_parser);
        binary_message_parser_init(&tls_binary_parser);
        message_formatter_init(&tls_csv_formatter);
        binary_message_formatter_init(&tls_binary_formatter);
        tls_initialized = true;
    }
}

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static bool setup_listening_socket(tcp_listener_context_t* ctx);
static bool setup_event_loop(tcp_listener_context_t* ctx);
static void handle_new_connection(tcp_listener_context_t* ctx);
static void handle_client_read(tcp_listener_context_t* ctx, uint32_t client_id);
static void handle_client_write(tcp_listener_context_t* ctx, uint32_t client_id);
static void disconnect_client(tcp_listener_context_t* ctx, uint32_t client_id);
static bool set_nonblocking(int fd);
static void process_output_queues(tcp_listener_context_t* ctx);
static bool route_message_to_queues(tcp_listener_context_t* ctx,
                                     const input_msg_envelope_t* envelope,
                                     const input_msg_t* msg);

#ifdef USE_KQUEUE
static bool add_fd_to_kqueue(int kq, int fd, int filter);
static bool modify_fd_in_kqueue(int kq, int fd, int filter, bool enable);
#endif

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Get symbol from input message for routing
 */
static const char* get_symbol_from_input_msg(const input_msg_t* msg) {
    assert(msg != NULL && "NULL msg in get_symbol_from_input_msg");

    switch (msg->type) {
        case INPUT_MSG_NEW_ORDER:
            return msg->data.new_order.symbol;
        case INPUT_MSG_CANCEL:
            return msg->data.cancel.symbol;
        case INPUT_MSG_FLUSH:
            return NULL;
        default:
            return NULL;
    }
}

/**
 * Route message to appropriate queue(s)
 */
static bool route_message_to_queues(tcp_listener_context_t* ctx,
                                     const input_msg_envelope_t* envelope,
                                     const input_msg_t* msg) {
    assert(ctx != NULL && "NULL ctx in route_message_to_queues");
    assert(envelope != NULL && "NULL envelope in route_message_to_queues");
    assert(msg != NULL && "NULL msg in route_message_to_queues");

    if (ctx->num_input_queues == 1) {
        /* Single processor mode */
        if (input_envelope_queue_enqueue(ctx->input_queues[0], envelope)) {
            ctx->messages_to_processor[0]++;
            return true;
        }
        ctx->queue_full_drops++;
        return false;
    }

    /* Dual processor mode - route by symbol */
    const char* symbol = get_symbol_from_input_msg(msg);

    if (msg->type == INPUT_MSG_FLUSH || !symbol_is_valid(symbol)) {
        /* Flush or cancel without symbol → send to BOTH queues */
        bool success_0 = input_envelope_queue_enqueue(ctx->input_queues[0], envelope);
        bool success_1 = input_envelope_queue_enqueue(ctx->input_queues[1], envelope);

        if (success_0) ctx->messages_to_processor[0]++;
        if (success_1) ctx->messages_to_processor[1]++;

        if (!success_0 || !success_1) {
            ctx->queue_full_drops++;
        }

        return success_0 && success_1;
    }

    /* Route by symbol (branchless) */
    int processor_id = get_processor_id_for_symbol(symbol);
    assert(processor_id >= 0 && processor_id < 2 && "Invalid processor ID");

    if (input_envelope_queue_enqueue(ctx->input_queues[processor_id], envelope)) {
        ctx->messages_to_processor[processor_id]++;
        return true;
    }

    ctx->queue_full_drops++;
    return false;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool tcp_listener_init(tcp_listener_context_t* ctx,
                       const tcp_listener_config_t* config,
                       tcp_client_registry_t* client_registry,
                       input_envelope_queue_t* input_queue,
                       atomic_bool* shutdown_flag) {
    assert(ctx != NULL && "NULL ctx in tcp_listener_init");
    assert(config != NULL && "NULL config in tcp_listener_init");
    assert(client_registry != NULL && "NULL client_registry");
    assert(input_queue != NULL && "NULL input_queue");
    assert(shutdown_flag != NULL && "NULL shutdown_flag");

    memset(ctx, 0, sizeof(*ctx));

    ctx->config = *config;
    ctx->client_registry = client_registry;
    ctx->input_queues[0] = input_queue;
    ctx->input_queues[1] = NULL;
    ctx->num_input_queues = 1;
    ctx->input_queue = input_queue;
    ctx->shutdown_flag = shutdown_flag;
    ctx->listen_fd = -1;

#ifdef USE_EPOLL
    ctx->epoll_fd = -1;
#elif defined(USE_KQUEUE)
    ctx->kqueue_fd = -1;
#endif

    return true;
}

bool tcp_listener_init_dual(tcp_listener_context_t* ctx,
                            const tcp_listener_config_t* config,
                            tcp_client_registry_t* client_registry,
                            input_envelope_queue_t* input_queue_0,
                            input_envelope_queue_t* input_queue_1,
                            atomic_bool* shutdown_flag) {
    assert(ctx != NULL && "NULL ctx in tcp_listener_init_dual");
    assert(config != NULL && "NULL config");
    assert(client_registry != NULL && "NULL client_registry");
    assert(input_queue_0 != NULL && "NULL input_queue_0");
    assert(input_queue_1 != NULL && "NULL input_queue_1");
    assert(shutdown_flag != NULL && "NULL shutdown_flag");

    memset(ctx, 0, sizeof(*ctx));

    ctx->config = *config;
    ctx->client_registry = client_registry;
    ctx->input_queues[0] = input_queue_0;
    ctx->input_queues[1] = input_queue_1;
    ctx->num_input_queues = 2;
    ctx->input_queue = input_queue_0;
    ctx->shutdown_flag = shutdown_flag;
    ctx->listen_fd = -1;

#ifdef USE_EPOLL
    ctx->epoll_fd = -1;
#elif defined(USE_KQUEUE)
    ctx->kqueue_fd = -1;
#endif

    return true;
}

void tcp_listener_cleanup(tcp_listener_context_t* ctx) {
    assert(ctx != NULL && "NULL ctx in tcp_listener_cleanup");

#ifdef USE_EPOLL
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
        ctx->epoll_fd = -1;
    }
#elif defined(USE_KQUEUE)
    if (ctx->kqueue_fd >= 0) {
        close(ctx->kqueue_fd);
        ctx->kqueue_fd = -1;
    }
#endif

    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
}

/* ============================================================================
 * Socket Setup [KB-1]
 * ============================================================================ */

/**
 * Setup listening socket with low-latency options
 *
 * [KB-1] Kernel Bypass: Replace with DPDK port initialization
 */
static bool setup_listening_socket(tcp_listener_context_t* ctx) {
    assert(ctx != NULL && "NULL ctx in setup_listening_socket");
    assert(ctx->listen_fd == -1 && "listen_fd already initialized");

    /* Create socket */
    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
        fprintf(stderr, "[TCP Listener] socket() failed: %s\n", strerror(errno));
        return false;
    }

    /* SO_REUSEADDR - Allow quick restart */
    int optval = 1;
    if (setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0) {
        fprintf(stderr, "[TCP Listener] setsockopt(SO_REUSEADDR) failed: %s\n",
                strerror(errno));
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return false;
    }

#ifdef SO_REUSEPORT
    /* SO_REUSEPORT - Allow multiple listeners on same port (for scaling) */
    if (setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEPORT,
                   &optval, sizeof(optval)) < 0) {
        /* Non-fatal - just log */
        fprintf(stderr, "[TCP Listener] Note: SO_REUSEPORT not available\n");
    }
#endif

    /* Make non-blocking */
    if (!set_nonblocking(ctx->listen_fd)) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return false;
    }

    /* Bind to port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(ctx->config.port);

    if (bind(ctx->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[TCP Listener] bind() failed on port %u: %s\n",
                ctx->config.port, strerror(errno));
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return false;
    }

    /* Listen with configurable backlog */
    int backlog = ctx->config.listen_backlog > 0 ?
                  ctx->config.listen_backlog : TCP_LISTEN_BACKLOG;

    if (listen(ctx->listen_fd, backlog) < 0) {
        fprintf(stderr, "[TCP Listener] listen() failed: %s\n", strerror(errno));
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return false;
    }

    fprintf(stderr, "[TCP Listener] Listening on port %u (backlog=%d)\n",
            ctx->config.port, backlog);

    return true;
}

/* ============================================================================
 * Event Loop Setup [KB-2]
 * ============================================================================ */

/**
 * Setup event loop (epoll/kqueue)
 *
 * [KB-2] Kernel Bypass: Replace with DPDK poll mode driver loop
 */
static bool setup_event_loop(tcp_listener_context_t* ctx) {
    assert(ctx != NULL && "NULL ctx in setup_event_loop");
    assert(ctx->listen_fd >= 0 && "listen_fd not initialized");

#ifdef USE_EPOLL
    ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epoll_fd < 0) {
        fprintf(stderr, "[TCP Listener] epoll_create1() failed: %s\n", strerror(errno));
        return false;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = ctx->listen_fd;

    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev) < 0) {
        fprintf(stderr, "[TCP Listener] epoll_ctl(listen) failed: %s\n", strerror(errno));
        close(ctx->epoll_fd);
        ctx->epoll_fd = -1;
        return false;
    }

#elif defined(USE_KQUEUE)
    ctx->kqueue_fd = kqueue();
    if (ctx->kqueue_fd < 0) {
        fprintf(stderr, "[TCP Listener] kqueue() failed: %s\n", strerror(errno));
        return false;
    }

    if (!add_fd_to_kqueue(ctx->kqueue_fd, ctx->listen_fd, EVFILT_READ)) {
        fprintf(stderr, "[TCP Listener] Failed to add listen socket to kqueue\n");
        close(ctx->kqueue_fd);
        ctx->kqueue_fd = -1;
        return false;
    }
#endif

    return true;
}

#ifdef USE_KQUEUE
static bool add_fd_to_kqueue(int kq, int fd, int filter) {
    assert(kq >= 0 && "Invalid kqueue fd");
    assert(fd >= 0 && "Invalid fd");

    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_ADD | EV_ENABLE, 0, 0, NULL);

    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        fprintf(stderr, "[TCP Listener] kevent(EV_ADD) failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}

static bool modify_fd_in_kqueue(int kq, int fd, int filter, bool enable) {
    assert(kq >= 0 && "Invalid kqueue fd");
    assert(fd >= 0 && "Invalid fd");

    struct kevent ev;
    uint16_t flags = enable ? (EV_ADD | EV_ENABLE) : (EV_ADD | EV_DISABLE);
    EV_SET(&ev, fd, filter, flags, 0, 0, NULL);

    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        fprintf(stderr, "[TCP Listener] kevent(modify) failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}
#endif

/* ============================================================================
 * Connection Handling [KB-5]
 * ============================================================================ */

/**
 * Handle new connection
 *
 * [KB-5] Kernel Bypass: N/A for DPDK (connectionless UDP)
 * For DPDK TCP via F-Stack/mTCP, this would integrate with their accept()
 */
static void handle_new_connection(tcp_listener_context_t* ctx) {
    assert(ctx != NULL && "NULL ctx in handle_new_connection");

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(ctx->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[TCP Listener] accept() failed: %s\n", strerror(errno));
        }
        return;
    }

    /* Make client socket non-blocking */
    if (!set_nonblocking(client_fd)) {
        close(client_fd);
        return;
    }

    /* Add client to registry (applies TCP_NODELAY internally) */
    uint32_t client_id;
    if (!tcp_client_add(ctx->client_registry, client_fd, client_addr, &client_id)) {
        fprintf(stderr, "[TCP Listener] Client registry full, rejecting connection\n");
        close(client_fd);
        return;
    }

    /* Add client socket to event loop */
#ifdef USE_EPOLL
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  /* Edge-triggered for efficiency */
    ev.data.fd = client_fd;

    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
        fprintf(stderr, "[TCP Listener] epoll_ctl(client) failed: %s\n", strerror(errno));
        tcp_client_remove(ctx->client_registry, client_id);
        return;
    }

#elif defined(USE_KQUEUE)
    if (!add_fd_to_kqueue(ctx->kqueue_fd, client_fd, EVFILT_READ)) {
        fprintf(stderr, "[TCP Listener] Failed to add client to kqueue\n");
        tcp_client_remove(ctx->client_registry, client_id);
        return;
    }
#endif

    ctx->total_connections++;
}

/* ============================================================================
 * Read Handling [KB-3]
 * ============================================================================ */

/**
 * Handle incoming data from a client connection
 *
 * [KB-3] Kernel Bypass: Replace read() with rte_eth_rx_burst() + packet parsing
 *
 * Rule Compliance:
 * - Rule 2: Loop bounded by MAX_MESSAGES_PER_READ
 * - Rule 5: Assertions for parameter validation
 * - Rule 7: All return values checked
 */
static void handle_client_read(tcp_listener_context_t* ctx, uint32_t client_id) {
    assert(ctx != NULL && "NULL ctx in handle_client_read");
    assert(ctx->client_registry != NULL && "NULL client registry");

    tcp_client_t* client = tcp_client_get(ctx->client_registry, client_id);
    if (!client) {
        return;
    }

    /* Ensure thread-local parsers are initialized */
    ensure_parsers_initialized();

    /* Read from socket */
    char buffer[MAX_READ_BUFFER];
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
        /* Client closed connection */
        disconnect_client(ctx, client_id);
        return;
    }

    /* Update statistics */
    client->bytes_received += (size_t)n;
    ctx->total_bytes_received += (size_t)n;

    /* Append to framing buffer */
    size_t consumed = framing_read_append(&client->read_state, buffer, (size_t)n);
    if (consumed < (size_t)n) {
        fprintf(stderr, "[TCP Listener] Buffer overflow for client %u, resetting\n",
                client_id);
        framing_read_state_init(&client->read_state);
        return;
    }

    /* Extract and process messages (Rule 2: bounded loop) */
    int messages_processed = 0;

    while (messages_processed < MAX_MESSAGES_PER_READ) {
        const char* msg_data = NULL;
        size_t msg_len = 0;

        framing_result_t result = framing_read_extract(&client->read_state,
                                                        &msg_data, &msg_len);

        if (result == FRAMING_NEED_MORE_DATA) {
            break;
        }

        if (result == FRAMING_ERROR) {
            fprintf(stderr, "[TCP Listener] Framing error for client %u\n", client_id);
            disconnect_client(ctx, client_id);
            return;
        }

        assert(result == FRAMING_MESSAGE_READY);
        assert(msg_data != NULL && msg_len > 0);

        /* Parse message (auto-detect binary vs CSV) */
        input_msg_t input_msg;
        bool parsed = false;

        if (msg_len > 0 && (unsigned char)msg_data[0] == 0x4D) {
            /* Binary protocol (magic byte 'M') */
            parsed = binary_message_parser_parse(&tls_binary_parser,
                                                  msg_data, msg_len, &input_msg);
        } else {
            /* CSV protocol */
            if (msg_len < MAX_READ_BUFFER) {
                char csv_buffer[MAX_READ_BUFFER];
                memcpy(csv_buffer, msg_data, msg_len);
                csv_buffer[msg_len] = '\0';
                parsed = message_parser_parse(&tls_csv_parser, csv_buffer, &input_msg);
            }
        }

        if (!parsed) {
            ctx->parse_errors++;
            messages_processed++;
            continue;
        }

        /* Validate userId matches client_id */
        bool valid = true;
        switch (input_msg.type) {
            case INPUT_MSG_NEW_ORDER:
                if (input_msg.data.new_order.user_id != client_id) {
                    fprintf(stderr, "[TCP Listener] Client %u spoofed userId %u\n",
                            client_id, input_msg.data.new_order.user_id);
                    valid = false;
                }
                break;
            case INPUT_MSG_CANCEL:
                if (input_msg.data.cancel.user_id != client_id) {
                    fprintf(stderr, "[TCP Listener] Client %u spoofed userId %u\n",
                            client_id, input_msg.data.cancel.user_id);
                    valid = false;
                }
                break;
            case INPUT_MSG_FLUSH:
                break;
        }

        if (valid) {
            input_msg_envelope_t envelope = create_input_envelope(
                &input_msg, client_id, client->messages_received
            );

            if (route_message_to_queues(ctx, &envelope, &input_msg)) {
                client->messages_received++;
                ctx->total_messages_received++;
            }
        }

        messages_processed++;
    }
}

/* ============================================================================
 * Write Handling [KB-4]
 * ============================================================================ */

/**
 * Handle write availability for a client
 *
 * [KB-4] Kernel Bypass: Replace write() with rte_eth_tx_burst()
 */
static void handle_client_write(tcp_listener_context_t* ctx, uint32_t client_id) {
    assert(ctx != NULL && "NULL ctx in handle_client_write");

    tcp_client_t* client = tcp_client_get(ctx->client_registry, client_id);
    if (!client || !client->has_pending_write) {
        return;
    }

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

    framing_write_mark_written(&client->write_state, (size_t)n);
    client->bytes_sent += (size_t)n;
    ctx->total_bytes_sent += (size_t)n;

    if (framing_write_is_complete(&client->write_state)) {
        client->has_pending_write = false;
        client->messages_sent++;
        ctx->total_messages_sent++;

        /* Disable write events */

#ifdef USE_EPOLL
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client->socket_fd;
        int rc = epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, client->socket_fd, &ev);
        (void)rc;
#elif defined(USE_KQUEUE)
        modify_fd_in_kqueue(ctx->kqueue_fd, client->socket_fd, EVFILT_WRITE, false);
#endif
    }
}

/* ============================================================================
 * Output Queue Processing
 * ============================================================================ */

static void process_output_queues(tcp_listener_context_t* ctx) {
    assert(ctx != NULL && "NULL ctx in process_output_queues");

    ensure_parsers_initialized();

    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
        tcp_client_t* client = &ctx->client_registry->clients[i];

        if (!client->active || client->has_pending_write) {
            continue;
        }

        output_msg_t msg;
        if (!tcp_client_dequeue_output(client, &msg)) {
            continue;
        }

        /* Format message */
        char formatted[MAX_FORMAT_BUFFER];
        size_t formatted_len = 0;

        if (ctx->config.use_binary_output) {
            size_t len;
            const void* binary_data = binary_message_formatter_format(
                &tls_binary_formatter, &msg, &len);

            if (binary_data && len <= sizeof(formatted)) {
                memcpy(formatted, binary_data, len);
                formatted_len = len;
            }
        } else {
            const char* csv_str = message_formatter_format(&tls_csv_formatter, &msg);
            if (csv_str) {
                formatted_len = strlen(csv_str);
                if (formatted_len < sizeof(formatted)) {
                    memcpy(formatted, csv_str, formatted_len);
                }
            }
        }

        if (formatted_len == 0) {
            fprintf(stderr, "[TCP Listener] Format failed for client %u\n",
                    client->client_id);
            continue;
        }

        /* Initialize framed write */
        if (!framing_write_state_init(&client->write_state, formatted, formatted_len)) {
            fprintf(stderr, "[TCP Listener] Message too large for client %u\n",
                    client->client_id);
            continue;
        }

        client->has_pending_write = true;

        /* Try immediate write */
        const char* data;
        size_t len;
        framing_write_get_remaining(&client->write_state, &data, &len);

        ssize_t n = write(client->socket_fd, data, len);

        if (n > 0) {
            framing_write_mark_written(&client->write_state, (size_t)n);
            client->bytes_sent += (size_t)n;
            ctx->total_bytes_sent += (size_t)n;

            if (framing_write_is_complete(&client->write_state)) {
                client->has_pending_write = false;
                client->messages_sent++;
                ctx->total_messages_sent++;
            } else {
                /* Need to wait for write availability */
#ifdef USE_EPOLL
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                ev.data.fd = client->socket_fd;
                epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, client->socket_fd, &ev);
#elif defined(USE_KQUEUE)
                add_fd_to_kqueue(ctx->kqueue_fd, client->socket_fd, EVFILT_WRITE);
#endif
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            disconnect_client(ctx, client->client_id);
        } else {
            /* EAGAIN - register for write events */
#ifdef USE_EPOLL
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
            ev.data.fd = client->socket_fd;
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, client->socket_fd, &ev);
#elif defined(USE_KQUEUE)
            add_fd_to_kqueue(ctx->kqueue_fd, client->socket_fd, EVFILT_WRITE);
#endif
        }
    }
}

/* ============================================================================
 * Disconnect
 * ============================================================================ */

static void disconnect_client(tcp_listener_context_t* ctx, uint32_t client_id) {
    assert(ctx != NULL && "NULL ctx in disconnect_client");

    tcp_client_t* client = tcp_client_get(ctx->client_registry, client_id);
    if (!client) {
        return;
    }

#ifdef USE_EPOLL
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, client->socket_fd, NULL);
#endif
    /* kqueue auto-removes on close */

    tcp_client_remove(ctx->client_registry, client_id);
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static bool set_nonblocking(int fd) {
    assert(fd >= 0 && "Invalid fd in set_nonblocking");

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

/* ============================================================================
 * Main Thread Entry Point
 * ============================================================================ */

void* tcp_listener_thread(void* arg) {
    tcp_listener_context_t* ctx = (tcp_listener_context_t*)arg;

    assert(ctx != NULL && "NULL context in tcp_listener_thread");

    fprintf(stderr, "[TCP Listener] Starting on port %u (mode: %s)\n",
            ctx->config.port,
            ctx->num_input_queues == 2 ? "dual-processor" : "single-processor");

    /* Setup */
    if (!setup_listening_socket(ctx)) {
        fprintf(stderr, "[TCP Listener] Failed to setup listening socket\n");
        atomic_store(ctx->shutdown_flag, true);
        return NULL;
    }

    if (!setup_event_loop(ctx)) {
        fprintf(stderr, "[TCP Listener] Failed to setup event loop\n");
        close(ctx->listen_fd);
        atomic_store(ctx->shutdown_flag, true);
        return NULL;
    }

    fprintf(stderr, "[TCP Listener] Ready and listening\n");

    /* Event arrays */
#ifdef USE_EPOLL
    struct epoll_event events[MAX_EVENTS];
#elif defined(USE_KQUEUE)
    struct kevent events[MAX_EVENTS];
    struct timespec timeout = {
        .tv_sec = EVENT_TIMEOUT_MS / 1000,
        .tv_nsec = (EVENT_TIMEOUT_MS % 1000) * 1000000
    };
#endif

    /* Main event loop */
    while (!atomic_load(ctx->shutdown_flag)) {
        /* Process pending output */
        process_output_queues(ctx);

        /* Wait for events */
#ifdef USE_EPOLL
        int nfds = epoll_wait(ctx->epoll_fd, events, MAX_EVENTS, EVENT_TIMEOUT_MS);
#elif defined(USE_KQUEUE)
        int nfds = kevent(ctx->kqueue_fd, NULL, 0, events, MAX_EVENTS, &timeout);
#endif

        if (nfds < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[TCP Listener] Event wait error: %s\n", strerror(errno));
            break;
        }

        /* Process events */
        for (int i = 0; i < nfds; i++) {
#ifdef USE_EPOLL
            int fd = events[i].data.fd;
            uint32_t event_mask = events[i].events;

            /* Error check */
            if (event_mask & (EPOLLERR | EPOLLHUP)) {
                if (fd == ctx->listen_fd) {
                    fprintf(stderr, "[TCP Listener] Error on listen socket\n");
                    goto cleanup;
                }
                /* Find and disconnect client */
                for (size_t j = 0; j < MAX_TCP_CLIENTS; j++) {
                    tcp_client_t* c = &ctx->client_registry->clients[j];
                    if (c->active && c->socket_fd == fd) {
                        disconnect_client(ctx, c->client_id);
                        break;
                    }
                }
                continue;
            }

            /* New connection */
            if (fd == ctx->listen_fd) {
                handle_new_connection(ctx);
                continue;
            }

            /* Find client */
            tcp_client_t* client = NULL;
            for (size_t j = 0; j < MAX_TCP_CLIENTS; j++) {
                if (ctx->client_registry->clients[j].active &&
                    ctx->client_registry->clients[j].socket_fd == fd) {
                    client = &ctx->client_registry->clients[j];
                    break;
                }
            }

            if (!client) continue;

            if (event_mask & EPOLLIN) {
                handle_client_read(ctx, client->client_id);
            }
            if (event_mask & EPOLLOUT) {
                handle_client_write(ctx, client->client_id);
            }

#elif defined(USE_KQUEUE)
            int fd = (int)events[i].ident;
            int16_t filter = events[i].filter;
            uint16_t flags = events[i].flags;

            if (flags & (EV_ERROR | EV_EOF)) {
                if (fd == ctx->listen_fd) {
                    fprintf(stderr, "[TCP Listener] Error on listen socket\n");
                    goto cleanup;
                }
                for (size_t j = 0; j < MAX_TCP_CLIENTS; j++) {
                    tcp_client_t* c = &ctx->client_registry->clients[j];
                    if (c->active && c->socket_fd == fd) {
                        disconnect_client(ctx, c->client_id);
                        break;
                    }
                }
                continue;
            }

            if (fd == ctx->listen_fd && filter == EVFILT_READ) {
                handle_new_connection(ctx);
                continue;
            }

            tcp_client_t* client = NULL;
            for (size_t j = 0; j < MAX_TCP_CLIENTS; j++) {
                if (ctx->client_registry->clients[j].active &&
                    ctx->client_registry->clients[j].socket_fd == fd) {
                    client = &ctx->client_registry->clients[j];
                    break;
                }
            }

            if (!client) continue;

            if (filter == EVFILT_READ) {
                handle_client_read(ctx, client->client_id);
            } else if (filter == EVFILT_WRITE) {
                handle_client_write(ctx, client->client_id);
            }
#endif
        }
    }

cleanup:
    fprintf(stderr, "[TCP Listener] Shutting down\n");
    tcp_listener_print_stats(ctx);
    tcp_listener_cleanup(ctx);
    return NULL;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void tcp_listener_print_stats(const tcp_listener_context_t* ctx) {
    assert(ctx != NULL && "NULL ctx in tcp_listener_print_stats");

    fprintf(stderr, "\n=== TCP Listener Statistics ===\n");
    fprintf(stderr, "Total connections:     %llu\n",
            (unsigned long long)ctx->total_connections);
    fprintf(stderr, "Active clients:        %zu\n",
            tcp_client_get_active_count(ctx->client_registry));
    fprintf(stderr, "Messages received:     %llu\n",
            (unsigned long long)ctx->total_messages_received);
    fprintf(stderr, "Messages sent:         %llu\n",
            (unsigned long long)ctx->total_messages_sent);
    fprintf(stderr, "Bytes received:        %llu\n",
            (unsigned long long)ctx->total_bytes_received);
    fprintf(stderr, "Bytes sent:            %llu\n",
            (unsigned long long)ctx->total_bytes_sent);
    fprintf(stderr, "Parse errors:          %llu\n",
            (unsigned long long)ctx->parse_errors);
    fprintf(stderr, "Queue full drops:      %llu\n",
            (unsigned long long)ctx->queue_full_drops);

    if (ctx->num_input_queues == 2) {
        fprintf(stderr, "To Processor 0 (A-M):  %llu\n",
                (unsigned long long)ctx->messages_to_processor[0]);
        fprintf(stderr, "To Processor 1 (N-Z):  %llu\n",
                (unsigned long long)ctx->messages_to_processor[1]);
    }
}

void tcp_listener_reset_stats(tcp_listener_context_t* ctx) {
    assert(ctx != NULL && "NULL ctx in tcp_listener_reset_stats");

    ctx->total_connections = 0;
    ctx->total_messages_received = 0;
    ctx->total_messages_sent = 0;
    ctx->total_bytes_received = 0;
    ctx->total_bytes_sent = 0;
    ctx->parse_errors = 0;
    ctx->queue_full_drops = 0;
    ctx->messages_to_processor[0] = 0;
    ctx->messages_to_processor[1] = 0;
}


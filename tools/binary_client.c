#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <netdb.h>

#define BINARY_MAGIC 0x4D
#define BINARY_SYMBOL_LEN 8
#define MAX_MSG_SIZE 256
#define FRAME_HEADER_SIZE 4

/* Binary new order message */
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  msg_type;
    uint32_t user_id;
    char     symbol[BINARY_SYMBOL_LEN];
    uint32_t price;
    uint32_t quantity;
    uint8_t  side;
    uint32_t user_order_id;
} binary_new_order_t;

/* Binary cancel message */
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  msg_type;
    uint32_t user_id;
    uint32_t user_order_id;
} binary_cancel_t;

/* Binary flush message */
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  msg_type;
} binary_flush_t;

/* Client context */
typedef struct {
    int sock;
    bool use_tcp;
    bool use_csv;
    struct sockaddr_in server;
    volatile bool running;
} client_context_t;

static client_context_t* g_ctx = NULL;

/* stdout print lock to prevent interleaving */
static pthread_mutex_t g_print_mtx = PTHREAD_MUTEX_INITIALIZER;

static void safe_printf(const char *fmt, ...) {
    va_list args;
    pthread_mutex_lock(&g_print_mtx);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&g_print_mtx);
}

/* Signal handler for graceful shutdown */
void signal_handler(int sig) {
    (void)sig;
    if (g_ctx) {
        safe_printf("\n\nShutting down gracefully...\n");
        g_ctx->running = false;
    }
}

/* Helper to safely copy symbol with padding */
static void copy_symbol(char* dest, const char* src) {
    size_t len = strlen(src);
    if (len > BINARY_SYMBOL_LEN) {
        len = BINARY_SYMBOL_LEN;
    }
    memcpy(dest, src, len);
    if (len < BINARY_SYMBOL_LEN) {
        memset(dest + len, 0, BINARY_SYMBOL_LEN - len);
    }
}

/* Frame message for TCP */
static bool frame_message_tcp(const void* msg_data, size_t msg_len,
                               char* output, size_t* output_len) {
    if (msg_len > MAX_MSG_SIZE) {
        return false;
    }

    uint32_t length_be = htonl((uint32_t)msg_len);
    memcpy(output, &length_be, FRAME_HEADER_SIZE);
    memcpy(output + FRAME_HEADER_SIZE, msg_data, msg_len);

    *output_len = FRAME_HEADER_SIZE + msg_len;
    return true;
}

/* Send data via TCP or UDP */
static ssize_t send_data(client_context_t* ctx, const void* data, size_t len) {
    if (ctx->use_tcp) {
        char framed[MAX_MSG_SIZE + FRAME_HEADER_SIZE];
        size_t framed_len;

        if (!frame_message_tcp(data, len, framed, &framed_len)) {
            fprintf(stderr, "Error: Message too large\n");
            return -1;
        }

        return send(ctx->sock, framed, framed_len, 0);
    } else {
        return sendto(ctx->sock, data, len, 0,
                     (struct sockaddr*)&ctx->server, sizeof(ctx->server));
    }
}

/* Send new order - Binary */
static void send_new_order_binary(client_context_t* ctx,
                                   uint32_t user_id, const char* symbol,
                                   uint32_t price, uint32_t qty,
                                   char side, uint32_t order_id) {
    binary_new_order_t msg = {0};
    msg.magic = BINARY_MAGIC;
    msg.msg_type = 'N';
    msg.user_id = htonl(user_id);
    copy_symbol(msg.symbol, symbol);
    msg.price = htonl(price);
    msg.quantity = htonl(qty);
    msg.side = side;
    msg.user_order_id = htonl(order_id);

    send_data(ctx, &msg, sizeof(msg));
}

/* Send new order - CSV */
static void send_new_order_csv(client_context_t* ctx,
                                uint32_t user_id, const char* symbol,
                                uint32_t price, uint32_t qty,
                                char side, uint32_t order_id) {
    char msg[MAX_MSG_SIZE];
    int len = snprintf(msg, sizeof(msg), "N,%u,%s,%u,%u,%c,%u\n",
                      user_id, symbol, price, qty, side, order_id);
    send_data(ctx, msg, (size_t)len);
}

/* Send cancel - Binary */
static void send_cancel_binary(client_context_t* ctx,
                                uint32_t user_id, uint32_t order_id) {
    binary_cancel_t msg = {0};
    msg.magic = BINARY_MAGIC;
    msg.msg_type = 'C';
    msg.user_id = htonl(user_id);
    msg.user_order_id = htonl(order_id);
    send_data(ctx, &msg, sizeof(msg));
}

/* Send cancel - CSV */
static void send_cancel_csv(client_context_t* ctx,
                             uint32_t user_id, uint32_t order_id) {
    char msg[MAX_MSG_SIZE];
    int len = snprintf(msg, sizeof(msg), "C,%u,%u\n", user_id, order_id);
    send_data(ctx, msg, (size_t)len);
}

/* Send flush - Binary */
static void send_flush_binary(client_context_t* ctx) {
    binary_flush_t msg = {0};
    msg.magic = BINARY_MAGIC;
    msg.msg_type = 'F';
    send_data(ctx, &msg, sizeof(msg));
}

/* Send flush - CSV */
static void send_flush_csv(client_context_t* ctx) {
    char msg[MAX_MSG_SIZE];
    int len = snprintf(msg, sizeof(msg), "F\n");
    send_data(ctx, msg, (size_t)len);
}

/* Wrapper functions */
static void send_new_order(client_context_t* ctx,
                           uint32_t user_id, const char* symbol,
                           uint32_t price, uint32_t qty,
                           char side, uint32_t order_id) {
    if (ctx->use_csv) {
        send_new_order_csv(ctx, user_id, symbol, price, qty, side, order_id);
    } else {
        send_new_order_binary(ctx, user_id, symbol, price, qty, side, order_id);
    }
}

static void send_cancel(client_context_t* ctx,
                        uint32_t user_id, uint32_t order_id) {
    if (ctx->use_csv) {
        send_cancel_csv(ctx, user_id, order_id);
    } else {
        send_cancel_binary(ctx, user_id, order_id);
    }
}

static void send_flush(client_context_t* ctx) {
    if (ctx->use_csv) {
        send_flush_csv(ctx);
    } else {
        send_flush_binary(ctx);
    }
}

/* Response reader thread (for TCP) */
void* response_reader_thread(void* arg) {
    client_context_t* ctx = (client_context_t*)arg;
    char buffer[4096];
    size_t buffer_pos = 0;

    while (ctx->running) {
        ssize_t n = recv(ctx->sock, buffer + buffer_pos,
                         sizeof(buffer) - buffer_pos - 1, 0);

        if (n > 0) {
            buffer_pos += (size_t)n;

            // Extract complete framed messages
            while (buffer_pos >= FRAME_HEADER_SIZE) {
                uint32_t msg_len_be;
                memcpy(&msg_len_be, buffer, FRAME_HEADER_SIZE);
                uint32_t msg_len = ntohl(msg_len_be);

                if (buffer_pos < FRAME_HEADER_SIZE + msg_len) {
                    break;
                }

                char* msg_start = buffer + FRAME_HEADER_SIZE;

                // Make a safe null-terminated copy for printing
                char tmp[MAX_MSG_SIZE + 1];
                size_t copy_len = msg_len;
                if (copy_len > MAX_MSG_SIZE) copy_len = MAX_MSG_SIZE;
                memcpy(tmp, msg_start, copy_len);
                tmp[copy_len] = '\0';

                // Print as a line (prefix helps separate from prompts)
                pthread_mutex_lock(&g_print_mtx);
                printf("[SERVER] %s", tmp);
                if (copy_len == 0 || tmp[copy_len-1] != '\n') {
                    printf("\n");
                }
                fflush(stdout);
                pthread_mutex_unlock(&g_print_mtx);

                size_t remaining = buffer_pos - (FRAME_HEADER_SIZE + msg_len);
                if (remaining > 0) {
                    memmove(buffer, buffer + FRAME_HEADER_SIZE + msg_len, remaining);
                }
                buffer_pos = remaining;
            }
        } else if (n == 0) {
            safe_printf("\n[Server closed connection]\n");
            ctx->running = false;
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            safe_printf("\n[recv error: %s]\n", strerror(errno));
            ctx->running = false;
            break;
        }
    }

    return NULL;
}

void print_help() {
    safe_printf("\nCommands:\n");
    safe_printf("  buy <symbol> <price> <qty> <order_id>   - Send buy order\n");
    safe_printf("  sell <symbol> <price> <qty> <order_id>  - Send sell order\n");
    safe_printf("  cancel <order_id>                      - Cancel order\n");
    safe_printf("  flush                                  - Flush order book\n");
    safe_printf("  help                                   - Show this help\n");
    safe_printf("  quit                                   - Exit\n\n");
}

/* Run interactive mode (TCP only) */
void run_interactive_mode(client_context_t* ctx) {
    char line[256];
    uint32_t user_id = 1;

    while (ctx->running) {
        pthread_mutex_lock(&g_print_mtx);
        printf("> ");
        fflush(stdout);
        pthread_mutex_unlock(&g_print_mtx);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        char symbol[16];
        uint32_t price, qty, order_id;

        if (strncmp(line, "buy ", 4) == 0) {
            if (sscanf(line, "buy %15s %u %u %u", symbol, &price, &qty, &order_id) == 4) {
                send_new_order(ctx, user_id, symbol, price, qty, 'B', order_id);
                safe_printf("Sent: BUY %s %u @ %u (order %u)\n", symbol, qty, price, order_id);
            } else {
                safe_printf("Usage: buy <symbol> <price> <qty> <order_id>\n");
            }
        } else if (strncmp(line, "sell ", 5) == 0) {
            if (sscanf(line, "sell %15s %u %u %u", symbol, &price, &qty, &order_id) == 4) {
                send_new_order(ctx, user_id, symbol, price, qty, 'S', order_id);
                safe_printf("Sent: SELL %s %u @ %u (order %u)\n", symbol, qty, price, order_id);
            } else {
                safe_printf("Usage: sell <symbol> <price> <qty> <order_id>\n");
            }
        } else if (strncmp(line, "cancel ", 7) == 0) {
            if (sscanf(line, "cancel %u", &order_id) == 1) {
                send_cancel(ctx, user_id, order_id);
                safe_printf("Sent: CANCEL order %u\n", order_id);
            } else {
                safe_printf("Usage: cancel <order_id>\n");
            }
        } else if (strncmp(line, "flush", 5) == 0) {
            send_flush(ctx);
            safe_printf("Sent: FLUSH\n");
        } else if (strncmp(line, "help", 4) == 0) {
            print_help();
        } else if (strncmp(line, "quit", 4) == 0) {
            break;
        } else if (line[0] != '\n') {
            safe_printf("Unknown command. Type 'help' for commands.\n");
        }
    }
}

/* Run scenario (UDP runs and exits, TCP runs then enters interactive) */
void run_scenario(client_context_t* ctx, int scenario) {
    struct timespec ts = {0, 100000000L};  // 100ms
    uint32_t user_id = 1;

    safe_printf("\n=== Running Scenario %d ===\n", scenario);

    switch (scenario) {
        case 1:
            safe_printf("Scenario 1: Simple Orders\n");
            send_new_order(ctx, user_id, "IBM", 100, 50, 'B', 1);
            safe_printf("Sent: NEW IBM B 50 @ 100 (order 1)\n");
            nanosleep(&ts, NULL);
            send_new_order(ctx, user_id, "IBM", 105, 50, 'S', 2);
            safe_printf("Sent: NEW IBM S 50 @ 105 (order 2)\n");
            nanosleep(&ts, NULL);
            send_flush(ctx);
            safe_printf("Sent: FLUSH\n");
            break;

        case 2:
            safe_printf("Scenario 2: Trade\n");
            send_new_order(ctx, user_id, "IBM", 100, 50, 'B', 1);
            safe_printf("Sent: NEW IBM B 50 @ 100 (order 1)\n");
            nanosleep(&ts, NULL);
            send_new_order(ctx, user_id, "IBM", 100, 50, 'S', 2);
            safe_printf("Sent: NEW IBM S 50 @ 100 (order 2)\n");
            nanosleep(&ts, NULL);
            send_flush(ctx);
            safe_printf("Sent: FLUSH\n");
            break;

        case 3:
            safe_printf("Scenario 3: Cancel\n");
            send_new_order(ctx, user_id, "IBM", 100, 50, 'B', 1);
            safe_printf("Sent: NEW IBM B 50 @ 100 (order 1)\n");
            nanosleep(&ts, NULL);
            send_new_order(ctx, user_id, "IBM", 105, 50, 'S', 2);
            safe_printf("Sent: NEW IBM S 50 @ 105 (order 2)\n");
            nanosleep(&ts, NULL);
            send_cancel(ctx, user_id, 1);
            safe_printf("Sent: CANCEL order 1\n");
            nanosleep(&ts, NULL);
            send_flush(ctx);
            safe_printf("Sent: FLUSH\n");
            break;

        default:
            safe_printf("Unknown scenario: %d\n", scenario);
    }

    safe_printf("=== Scenario Complete ===\n\n");

    // Give server time to respond
    nanosleep(&ts, NULL);
    nanosleep(&ts, NULL);
}

void print_usage(const char* progname) {
    printf("Usage: %s <port> [scenario] [options]\n", progname);
    printf("\nOptions:\n");
    printf("  --tcp           Use TCP (default: UDP)\n");
    printf("  --csv           Use CSV protocol (default: binary)\n");
    printf("  --host <ip>     Server IP/hostname (default: 127.0.0.1)\n");
    printf("\nBehavior:\n");
    printf("  UDP:              Run scenario and exit (fire-and-forget)\n");
    printf("  TCP + scenario:   Run scenario, then enter interactive mode\n");
    printf("  TCP no scenario:  Enter interactive mode directly\n");
    printf("\nScenarios:\n");
    printf("  1 - Simple order test\n");
    printf("  2 - Trade test\n");
    printf("  3 - Cancel test\n");
    printf("\nExamples:\n");
    printf("  %s 1234 --tcp                   # TCP interactive\n", progname);
    printf("  %s 1234 2 --tcp --csv           # TCP scenario 2, CSV protocol\n", progname);
    printf("  %s 1234 1                       # UDP scenario 1, binary protocol\n", progname);
    printf("  %s 1234 1 --host 192.168.1.50   # UDP to remote server\n", progname);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    int scenario = 0;
    bool use_tcp = false;
    bool use_csv = false;
    const char *host = "127.0.0.1";

    // Parse arguments
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--tcp") == 0) {
            use_tcp = true;
        } else if (strcmp(argv[i], "--csv") == 0) {
            use_csv = true;
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (scenario == 0 && atoi(argv[i]) > 0) {
            scenario = atoi(argv[i]);
        }
    }

    // Create socket
    int sock_type = use_tcp ? SOCK_STREAM : SOCK_DGRAM;
    int sock = socket(AF_INET, sock_type, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // Setup server address
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &server.sin_addr) != 1) {
        // DNS resolve (portable)
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;      // IPv4 only (matches server)
        hints.ai_socktype = sock_type;  // TCP or UDP

        int rc = getaddrinfo(host, NULL, &hints, &res);
        if (rc != 0 || !res) {
            fprintf(stderr, "Invalid host '%s': %s\n",
                    host, rc == 0 ? "no results" : gai_strerror(rc));
            close(sock);
            return 1;
        }

        struct sockaddr_in *addr_in = (struct sockaddr_in*)res->ai_addr;
        server.sin_addr = addr_in->sin_addr;
        freeaddrinfo(res);
    }

    // Connect if TCP
    if (use_tcp) {
        if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
            perror("connect");
            close(sock);
            return 1;
        }
    }

    safe_printf("=== Trading Client ===\n");
    safe_printf("Mode:     %s\n", use_tcp ? "TCP" : "UDP");
    safe_printf("Protocol: %s\n", use_csv ? "CSV" : "Binary");
    safe_printf("Server:   %s:%d\n", host, port);

    // Setup context
    client_context_t ctx = {
        .sock = sock,
        .use_tcp = use_tcp,
        .use_csv = use_csv,
        .server = server,
        .running = true
    };

    g_ctx = &ctx;
    signal(SIGINT, signal_handler);

    // Start response reader thread for TCP
    pthread_t reader_thread;
    if (use_tcp) {
        pthread_create(&reader_thread, NULL, response_reader_thread, &ctx);
    }

    // Run scenario if specified
    if (scenario > 0) {
        run_scenario(&ctx, scenario);
    }

    // TCP: Enter interactive mode (even after scenario)
    // UDP: Exit immediately after scenario
    if (use_tcp) {
        if (scenario == 0) {
            safe_printf("\n");
        }
        print_help();
        run_interactive_mode(&ctx);

        ctx.running = false;
        pthread_join(reader_thread, NULL);
    }

    close(sock);
    safe_printf("\nDisconnected.\n");
    return 0;
}


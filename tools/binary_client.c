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

/* Signal handler for graceful shutdown */
void signal_handler(int sig) {
    (void)sig;
    if (g_ctx) {
        printf("\n\nShutting down gracefully...\n");
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
    send_data(ctx, msg, len);
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
    send_data(ctx, msg, len);
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
    send_data(ctx, msg, len);
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

/* Response reader thread (for TCP interactive mode) */
void* response_reader_thread(void* arg) {
    client_context_t* ctx = (client_context_t*)arg;
    char buffer[4096];
    size_t buffer_pos = 0;
    
    while (ctx->running) {
        ssize_t n = recv(ctx->sock, buffer + buffer_pos, sizeof(buffer) - buffer_pos - 1, 0);
        
        if (n > 0) {
            buffer_pos += n;
            
            // Extract complete framed messages
            while (buffer_pos >= FRAME_HEADER_SIZE) {
                uint32_t msg_len_be;
                memcpy(&msg_len_be, buffer, FRAME_HEADER_SIZE);
                uint32_t msg_len = ntohl(msg_len_be);
                
                if (buffer_pos < FRAME_HEADER_SIZE + msg_len) {
                    break;
                }
                
                char* msg_start = buffer + FRAME_HEADER_SIZE;
                char saved_char = msg_start[msg_len];
                msg_start[msg_len] = '\0';
                printf("%s", msg_start);
                fflush(stdout);
                msg_start[msg_len] = saved_char;
                
                size_t remaining = buffer_pos - (FRAME_HEADER_SIZE + msg_len);
                if (remaining > 0) {
                    memmove(buffer, buffer + FRAME_HEADER_SIZE + msg_len, remaining);
                }
                buffer_pos = remaining;
            }
        } else if (n == 0) {
            printf("\n[Server closed connection]\n");
            ctx->running = false;
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ctx->running = false;
            break;
        }
    }
    
    return NULL;
}

void print_help() {
    printf("\nCommands:\n");
    printf("  buy <symbol> <price> <qty> <order_id>   - Send buy order\n");
    printf("  sell <symbol> <price> <qty> <order_id>  - Send sell order\n");
    printf("  cancel <order_id>                        - Cancel order\n");
    printf("  flush                                     - Flush order book\n");
    printf("  help                                      - Show this help\n");
    printf("  quit                                      - Exit\n");
    printf("\n");
}

/* Run interactive mode (TCP) */
void run_interactive_mode(client_context_t* ctx) {
    pthread_t reader_thread;
    pthread_create(&reader_thread, NULL, response_reader_thread, ctx);
    
    print_help();
    
    char line[256];
    uint32_t user_id = 1;
    
    while (ctx->running) {
        printf("> ");
        fflush(stdout);
        
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        
        char symbol[16];
        uint32_t price, qty, order_id;
        
        if (strncmp(line, "buy ", 4) == 0) {
            if (sscanf(line, "buy %s %u %u %u", symbol, &price, &qty, &order_id) == 4) {
                send_new_order(ctx, user_id, symbol, price, qty, 'B', order_id);
                printf("Sent: BUY %s %u @ %u (order %u)\n", symbol, qty, price, order_id);
            } else {
                printf("Usage: buy <symbol> <price> <qty> <order_id>\n");
            }
        } else if (strncmp(line, "sell ", 5) == 0) {
            if (sscanf(line, "sell %s %u %u %u", symbol, &price, &qty, &order_id) == 4) {
                send_new_order(ctx, user_id, symbol, price, qty, 'S', order_id);
                printf("Sent: SELL %s %u @ %u (order %u)\n", symbol, qty, price, order_id);
            } else {
                printf("Usage: sell <symbol> <price> <qty> <order_id>\n");
            }
        } else if (strncmp(line, "cancel ", 7) == 0) {
            if (sscanf(line, "cancel %u", &order_id) == 1) {
                send_cancel(ctx, user_id, order_id);
                printf("Sent: CANCEL order %u\n", order_id);
            } else {
                printf("Usage: cancel <order_id>\n");
            }
        } else if (strncmp(line, "flush", 5) == 0) {
            send_flush(ctx);
            printf("Sent: FLUSH\n");
        } else if (strncmp(line, "help", 4) == 0) {
            print_help();
        } else if (strncmp(line, "quit", 4) == 0) {
            break;
        } else if (line[0] != '\n') {
            printf("Unknown command. Type 'help' for commands.\n");
        }
    }
    
    ctx->running = false;
    pthread_join(reader_thread, NULL);
}

/* Run scenario mode (UDP or TCP one-shot) */
void run_scenario_mode(client_context_t* ctx, int scenario) {
    struct timespec ts = {0, 100000000L};  // 100ms
    uint32_t user_id = 1;
    
    switch (scenario) {
        case 1:
            printf("Scenario 1: Simple Orders\n");
            send_new_order(ctx, user_id, "IBM", 100, 50, 'B', 1);
            printf("Sent: NEW IBM B 50 @ 100 (order 1)\n");
            nanosleep(&ts, NULL);
            send_new_order(ctx, user_id, "IBM", 105, 50, 'S', 2);
            printf("Sent: NEW IBM S 50 @ 105 (order 2)\n");
            nanosleep(&ts, NULL);
            send_flush(ctx);
            printf("Sent: FLUSH\n");
            break;
            
        case 2:
            printf("Scenario 2: Trade\n");
            send_new_order(ctx, user_id, "IBM", 100, 50, 'B', 1);
            printf("Sent: NEW IBM B 50 @ 100 (order 1)\n");
            nanosleep(&ts, NULL);
            send_new_order(ctx, user_id, "IBM", 100, 50, 'S', 2);
            printf("Sent: NEW IBM S 50 @ 100 (order 2)\n");
            nanosleep(&ts, NULL);
            send_flush(ctx);
            printf("Sent: FLUSH\n");
            break;
            
        case 3:
            printf("Scenario 3: Cancel\n");
            send_new_order(ctx, user_id, "IBM", 100, 50, 'B', 1);
            printf("Sent: NEW IBM B 50 @ 100 (order 1)\n");
            nanosleep(&ts, NULL);
            send_new_order(ctx, user_id, "IBM", 105, 50, 'S', 2);
            printf("Sent: NEW IBM S 50 @ 105 (order 2)\n");
            nanosleep(&ts, NULL);
            send_cancel(ctx, user_id, 1);
            printf("Sent: CANCEL order 1\n");
            nanosleep(&ts, NULL);
            send_flush(ctx);
            printf("Sent: FLUSH\n");
            break;
            
        default:
            printf("Unknown scenario: %d\n", scenario);
    }
    
    printf("\nTest complete. Check server output.\n");
}

void print_usage(const char* progname) {
    printf("Usage: %s <port> [scenario] [options]\n", progname);
    printf("\nOptions:\n");
    printf("  --tcp         Use TCP (default: UDP)\n");
    printf("  --csv         Use CSV protocol (default: binary)\n");
    printf("\nModes:\n");
    printf("  TCP without scenario: Interactive mode (stay connected)\n");
    printf("  TCP with scenario:    Run scenario and exit\n");
    printf("  UDP (any):            Run scenario and exit\n");
    printf("\nScenarios:\n");
    printf("  1 - Simple order test\n");
    printf("  2 - Trade test\n");
    printf("  3 - Cancel test\n");
    printf("\nExamples:\n");
    printf("  %s 1234 --tcp              # TCP interactive mode\n", progname);
    printf("  %s 1234 2 --tcp --csv      # TCP scenario 2 and exit\n", progname);
    printf("  %s 1234 1                  # UDP scenario 1\n", progname);
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
    
    // Parse arguments
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--tcp") == 0) {
            use_tcp = true;
        } else if (strcmp(argv[i], "--csv") == 0) {
            use_csv = true;
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
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    // Connect if TCP
    if (use_tcp) {
        if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
            perror("connect");
            close(sock);
            return 1;
        }
    }
    
    printf("=== Trading Client ===\n");
    printf("Mode:     %s\n", use_tcp ? "TCP" : "UDP");
    printf("Protocol: %s\n", use_csv ? "CSV" : "Binary");
    printf("Server:   127.0.0.1:%d\n", port);
    
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
    
    // Decide mode: interactive (TCP without scenario) or scenario (UDP or TCP with scenario)
    if (use_tcp && scenario == 0) {
        // TCP interactive mode
        printf("Mode:     Interactive\n\n");
        run_interactive_mode(&ctx);
    } else {
        // Scenario mode (UDP or TCP one-shot)
        if (scenario == 0) scenario = 1;  // Default scenario
        printf("Scenario: %d\n\n", scenario);
        run_scenario_mode(&ctx, scenario);
    }
    
    close(sock);
    printf("\nDisconnected.\n");
    return 0;
}

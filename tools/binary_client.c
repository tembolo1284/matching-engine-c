#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <stdbool.h>

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
} client_context_t;

/* Helper to safely copy symbol with padding */
static void copy_symbol(char* dest, const char* src) {
    size_t len = strlen(src);
    if (len > BINARY_SYMBOL_LEN) {
        len = BINARY_SYMBOL_LEN;
    }
    memcpy(dest, src, len);
    /* Pad remaining with nulls */
    if (len < BINARY_SYMBOL_LEN) {
        memset(dest + len, 0, BINARY_SYMBOL_LEN - len);
    }
}

/* Frame message for TCP (add 4-byte length header) */
static bool frame_message_tcp(const void* msg_data, size_t msg_len,
                               char* output, size_t* output_len) {
    if (msg_len > MAX_MSG_SIZE) {
        return false;
    }
    
    // Write 4-byte length header (big-endian)
    uint32_t length_be = htonl((uint32_t)msg_len);
    memcpy(output, &length_be, FRAME_HEADER_SIZE);
    
    // Write message payload
    memcpy(output + FRAME_HEADER_SIZE, msg_data, msg_len);
    
    *output_len = FRAME_HEADER_SIZE + msg_len;
    return true;
}

/* Send data via TCP or UDP */
static ssize_t send_data(client_context_t* ctx, const void* data, size_t len) {
    if (ctx->use_tcp) {
        // TCP needs framing
        char framed[MAX_MSG_SIZE + FRAME_HEADER_SIZE];
        size_t framed_len;
        
        if (!frame_message_tcp(data, len, framed, &framed_len)) {
            fprintf(stderr, "Error: Message too large for framing\n");
            return -1;
        }
        
        return send(ctx->sock, framed, framed_len, 0);
    } else {
        // UDP sends raw
        return sendto(ctx->sock, data, len, 0,
                     (struct sockaddr*)&ctx->server, sizeof(ctx->server));
    }
}

/* Send new order - Binary protocol */
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
    
    printf("Sent: NEW %s %c %u @ %u (order %u)\n",
           symbol, side, qty, price, order_id);
}

/* Send new order - CSV protocol */
static void send_new_order_csv(client_context_t* ctx,
                                uint32_t user_id, const char* symbol,
                                uint32_t price, uint32_t qty,
                                char side, uint32_t order_id) {
    char msg[MAX_MSG_SIZE];
    int len = snprintf(msg, sizeof(msg), "N,%u,%s,%u,%u,%c,%u\n",
                      user_id, symbol, price, qty, side, order_id);
    
    send_data(ctx, msg, len);
    
    printf("Sent: NEW %s %c %u @ %u (order %u)\n",
           symbol, side, qty, price, order_id);
}

/* Send cancel - Binary protocol */
static void send_cancel_binary(client_context_t* ctx,
                                uint32_t user_id, uint32_t order_id) {
    binary_cancel_t msg = {0};
    msg.magic = BINARY_MAGIC;
    msg.msg_type = 'C';
    msg.user_id = htonl(user_id);
    msg.user_order_id = htonl(order_id);
    
    send_data(ctx, &msg, sizeof(msg));
    
    printf("Sent: CANCEL order %u\n", order_id);
}

/* Send cancel - CSV protocol */
static void send_cancel_csv(client_context_t* ctx,
                             uint32_t user_id, uint32_t order_id) {
    char msg[MAX_MSG_SIZE];
    int len = snprintf(msg, sizeof(msg), "C,%u,%u\n", user_id, order_id);
    
    send_data(ctx, msg, len);
    
    printf("Sent: CANCEL order %u\n", order_id);
}

/* Send flush - Binary protocol */
static void send_flush_binary(client_context_t* ctx) {
    binary_flush_t msg = {0};
    msg.magic = BINARY_MAGIC;
    msg.msg_type = 'F';
    
    send_data(ctx, &msg, sizeof(msg));
    
    printf("Sent: FLUSH\n");
}

/* Send flush - CSV protocol */
static void send_flush_csv(client_context_t* ctx) {
    char msg[MAX_MSG_SIZE];
    int len = snprintf(msg, sizeof(msg), "F\n");
    
    send_data(ctx, msg, len);
    
    printf("Sent: FLUSH\n");
}

/* Wrapper functions that choose protocol */
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

void print_usage(const char* progname) {
    printf("Usage: %s <port> <scenario> [options]\n", progname);
    printf("\nOptions:\n");
    printf("  --tcp         Use TCP (default: UDP)\n");
    printf("  --csv         Use CSV protocol (default: binary)\n");
    printf("\nScenarios:\n");
    printf("  1 - Simple order test\n");
    printf("  2 - Trade test\n");
    printf("  3 - Cancel test\n");
    printf("\nExamples:\n");
    printf("  %s 1234 1              # UDP + Binary\n", progname);
    printf("  %s 1234 2 --tcp        # TCP + Binary\n", progname);
    printf("  %s 1234 3 --csv        # UDP + CSV\n", progname);
    printf("  %s 1234 1 --tcp --csv  # TCP + CSV\n", progname);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    int scenario = atoi(argv[2]);
    
    /* Parse options */
    bool use_tcp = false;
    bool use_csv = false;
    
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--tcp") == 0) {
            use_tcp = true;
        } else if (strcmp(argv[i], "--csv") == 0) {
            use_csv = true;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    /* Create socket */
    int sock_type = use_tcp ? SOCK_STREAM : SOCK_DGRAM;
    int sock = socket(AF_INET, sock_type, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    /* Setup server address */
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    /* Connect if TCP */
    if (use_tcp) {
        if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
            perror("connect");
            close(sock);
            return 1;
        }
    }
    
    printf("=== Binary Protocol Test Client ===\n");
    printf("Mode:     %s\n", use_tcp ? "TCP" : "UDP");
    printf("Protocol: %s\n", use_csv ? "CSV" : "Binary");
    printf("Server:   127.0.0.1:%d\n", port);
    printf("Scenario: %d\n\n", scenario);
    
    /* Setup context */
    client_context_t ctx = {
        .sock = sock,
        .use_tcp = use_tcp,
        .use_csv = use_csv,
        .server = server
    };
    
    /* Sleep helper */
    struct timespec ts = {0, 100000000L}; // 100ms
    
    /* Run scenario */
    switch (scenario) {
        case 1:
            /* Simple order test */
            printf("Scenario 1: Simple Orders\n");
            send_new_order(&ctx, 1, "IBM", 100, 50, 'B', 1);
            nanosleep(&ts, NULL);
            send_new_order(&ctx, 2, "IBM", 105, 50, 'S', 2);
            nanosleep(&ts, NULL);
            send_flush(&ctx);
            break;
            
        case 2:
            /* Trade test */
            printf("Scenario 2: Trade\n");
            send_new_order(&ctx, 1, "IBM", 100, 50, 'B', 1);
            nanosleep(&ts, NULL);
            send_new_order(&ctx, 2, "IBM", 100, 50, 'S', 2);
            nanosleep(&ts, NULL);
            send_flush(&ctx);
            break;
            
        case 3:
            /* Cancel test */
            printf("Scenario 3: Cancel\n");
            send_new_order(&ctx, 1, "IBM", 100, 50, 'B', 1);
            nanosleep(&ts, NULL);
            send_new_order(&ctx, 2, "IBM", 105, 50, 'S', 2);
            nanosleep(&ts, NULL);
            send_cancel(&ctx, 1, 1);
            nanosleep(&ts, NULL);
            send_flush(&ctx);
            break;
            
        default:
            printf("Unknown scenario: %d\n", scenario);
            close(sock);
            return 1;
    }
    
    printf("\nTest complete. Check server output.\n");
    
    /* For TCP, give server time to respond */
    if (use_tcp) {
        nanosleep(&ts, NULL);
    }
    
    close(sock);
    return 0;
}

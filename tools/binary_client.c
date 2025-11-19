#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

#define BINARY_MAGIC 0x4D
#define BINARY_SYMBOL_LEN 8

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

void send_new_order(int sock, struct sockaddr_in* server, 
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
    
    sendto(sock, &msg, sizeof(msg), 0, 
           (struct sockaddr*)server, sizeof(*server));
    
    printf("Sent: NEW %s %c %u @ %u (order %u)\n", 
           symbol, side, qty, price, order_id);
}

void send_cancel(int sock, struct sockaddr_in* server,
                 uint32_t user_id, uint32_t order_id) {
    binary_cancel_t msg = {0};
    msg.magic = BINARY_MAGIC;
    msg.msg_type = 'C';
    msg.user_id = htonl(user_id);
    msg.user_order_id = htonl(order_id);
    
    sendto(sock, &msg, sizeof(msg), 0,
           (struct sockaddr*)server, sizeof(*server));
    
    printf("Sent: CANCEL order %u\n", order_id);
}

void send_flush(int sock, struct sockaddr_in* server) {
    binary_flush_t msg = {0};
    msg.magic = BINARY_MAGIC;
    msg.msg_type = 'F';
    
    sendto(sock, &msg, sizeof(msg), 0,
           (struct sockaddr*)server, sizeof(*server));
    
    printf("Sent: FLUSH\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <port> [test_scenario]\n", argv[0]);
        printf("Test scenarios:\n");
        printf("  1 - Simple order test\n");
        printf("  2 - Trade test\n");
        printf("  3 - Cancel test\n");
        return 1;
    }
    
    int port = atoi(argv[1]);
    int scenario = (argc > 2) ? atoi(argv[2]) : 1;
    
    /* Create UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    printf("=== Binary Protocol Test Client ===\n");
    printf("Connecting to localhost:%d\n", port);
    printf("Running scenario %d\n\n", scenario);
    
    struct timespec ts = {0, 100000000L};
    nanosleep(&ts, NULL);
    // usleep(100000);  /* 100ms delay */ deprecated way of sleeping in C
    
    switch (scenario) {
        case 1:
            /* Simple order test */
            printf("Scenario 1: Simple Orders\n");
            send_new_order(sock, &server, 1, "IBM", 100, 50, 'B', 1);
            nanosleep(&ts, NULL);
            send_new_order(sock, &server, 2, "IBM", 105, 50, 'S', 2);
            nanosleep(&ts, NULL);
            send_flush(sock, &server);
            break;
            
        case 2:
            /* Trade test */
            printf("Scenario 2: Trade\n");
            send_new_order(sock, &server, 1, "IBM", 100, 50, 'B', 1);
            nanosleep(&ts, NULL);
            send_new_order(sock, &server, 2, "IBM", 100, 50, 'S', 2);
            nanosleep(&ts, NULL);
            send_flush(sock, &server);
            break;
            
        case 3:
            /* Cancel test */
            printf("Scenario 3: Cancel\n");
            send_new_order(sock, &server, 1, "IBM", 100, 50, 'B', 1);
            nanosleep(&ts, NULL);
            send_new_order(sock, &server, 2, "IBM", 105, 50, 'S', 2);
            nanosleep(&ts, NULL);
            send_cancel(sock, &server, 1, 1);
            nanosleep(&ts, NULL);
            send_flush(sock, &server);
            break;
            
        default:
            printf("Unknown scenario\n");
    }
    
    printf("\nTest complete. Check server output.\n");
    
    close(sock);
    return 0;
}

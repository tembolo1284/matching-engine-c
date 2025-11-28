#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

#define MAX_MESSAGE_SIZE 2048
#define FRAME_HEADER_SIZE 4

static const struct timespec ts = {
    .tv_sec = 0,
    .tv_nsec = 100000
};

/* ============================================================================
 * Framing Protocol
 * ============================================================================ */

static bool send_framed_message(int sockfd, const char* msg, size_t msg_len) {
    char buffer[MAX_MESSAGE_SIZE + FRAME_HEADER_SIZE];
    
    uint32_t length_be = htonl((uint32_t)msg_len);
    memcpy(buffer, &length_be, FRAME_HEADER_SIZE);
    memcpy(buffer + FRAME_HEADER_SIZE, msg, msg_len);
    
    size_t total_len = FRAME_HEADER_SIZE + msg_len;
    size_t sent = 0;
    
    while (sent < total_len) {
        ssize_t n = write(sockfd, buffer + sent, total_len - sent);
        if (n < 0) {
            fprintf(stderr, "Write error: %s\n", strerror(errno));
            return false;
        }
        sent += n;
    }
    
    return true;
}

/*
 * Wrapper to receive a framed message with explicit bounds checking.
 * Marked noinline to prevent fortify false positives from aggressive inlining.
 */
__attribute__((noinline))
static bool recv_framed_message(int sockfd, char* msg_buffer, size_t buffer_size, size_t* msg_len) {
    uint32_t length_be;
    size_t received = 0;
    
    while (received < FRAME_HEADER_SIZE) {
        ssize_t n = read(sockfd, ((char*)&length_be) + received, 
                        FRAME_HEADER_SIZE - received);
        if (n <= 0) {
            return false;
        }
        received += (size_t)n;
    }
    
    uint32_t length = ntohl(length_be);
    if (length == 0 || length > buffer_size) {
        fprintf(stderr, "Invalid message length: %u bytes\n", length);
        return false;
    }
    
    received = 0;
    while (received < length) {
        ssize_t n = read(sockfd, msg_buffer + received, length - received);
        if (n <= 0) {
            return false;
        }
        received += (size_t)n;
    }
    
    *msg_len = (size_t)length;
    return true;
}

/* ============================================================================
 * Connection
 * ============================================================================ */

static int connect_to_server(const char* host, uint16_t port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(sockfd);
        return -1;
    }
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect to %s:%u: %s\n", 
                host, port, strerror(errno));
        close(sockfd);
        return -1;
    }
    
    fprintf(stderr, "Connected to %s:%u\n", host, port);
    return sockfd;
}

/* ============================================================================
 * Helper: Strip spaces from CSV message
 * ============================================================================
 * Converts "N, 1, IBM, 100, 50, B, 1" to "N,1,IBM,100,50,B,1"
 */
static void strip_csv_spaces(char* dest, const char* src, size_t dest_size) {
    size_t j = 0;
    bool after_comma = false;
    
    for (size_t i = 0; src[i] != '\0' && j < dest_size - 1; i++) {
        if (src[i] == ',') {
            dest[j++] = ',';
            after_comma = true;
        } else if (src[i] == ' ' && after_comma) {
            /* Skip spaces after commas */
            continue;
        } else {
            dest[j++] = src[i];
            after_comma = false;
        }
    }
    dest[j] = '\0';
}

/* ============================================================================
 * Test Scenarios (no spaces in CSV!)
 * ============================================================================ */

static void run_scenario_1(int sockfd) {
    fprintf(stderr, "\n=== Scenario 1: Simple Orders ===\n");
    
    const char* order1 = "N,1,IBM,100,50,B,1\n";
    fprintf(stderr, "Sending: %s", order1);
    send_framed_message(sockfd, order1, strlen(order1));
    
    sleep(1);
    
    const char* order2 = "N,1,IBM,105,50,S,2\n";
    fprintf(stderr, "Sending: %s", order2);
    send_framed_message(sockfd, order2, strlen(order2));
    
    sleep(1);
    
    const char* flush = "F\n";
    fprintf(stderr, "Sending: FLUSH\n");
    send_framed_message(sockfd, flush, strlen(flush));
    
    sleep(1);
}

static void run_scenario_2(int sockfd) {
    fprintf(stderr, "\n=== Scenario 2: Matching Trade ===\n");
    
    const char* buy = "N,1,IBM,100,50,B,1\n";
    fprintf(stderr, "Sending BUY: %s", buy);
    send_framed_message(sockfd, buy, strlen(buy));
    
    sleep(1);
    
    const char* sell = "N,1,IBM,100,50,S,2\n";
    fprintf(stderr, "Sending SELL: %s", sell);
    send_framed_message(sockfd, sell, strlen(sell));
    
    sleep(1);
}

static void run_scenario_3(int sockfd) {
    fprintf(stderr, "\n=== Scenario 3: Cancel Order ===\n");
    
    const char* order = "N,1,IBM,100,50,B,1\n";
    fprintf(stderr, "Sending: %s", order);
    send_framed_message(sockfd, order, strlen(order));
    
    sleep(1);
    
    const char* cancel = "C,1,1\n";
    fprintf(stderr, "Sending CANCEL: %s", cancel);
    send_framed_message(sockfd, cancel, strlen(cancel));
    
    sleep(1);
}

static void run_interactive_mode(int sockfd) {
    fprintf(stderr, "\n=== Interactive Mode ===\n");
    fprintf(stderr, "Enter orders (or 'quit' to exit):\n");
    fprintf(stderr, "Format (spaces optional, will be stripped):\n");
    fprintf(stderr, "  N,1,IBM,100,50,B,1     (new order: type,user,symbol,price,qty,side,order_id)\n");
    fprintf(stderr, "  C,1,1                  (cancel: type,user,order_id)\n");
    fprintf(stderr, "  F                      (flush)\n\n");
    fprintf(stderr, "Quick commands:\n");
    fprintf(stderr, "  buy IBM 100 50         (shorthand for buy order)\n");
    fprintf(stderr, "  sell IBM 100 50        (shorthand for sell order)\n");
    fprintf(stderr, "  flush                  (send flush)\n\n");
    
    char line[1024];
    char cleaned[1024];
    static int order_id = 1;
    
    while (1) {
        fprintf(stderr, "> ");
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        
        /* Trim trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        
        /* Skip empty lines */
        if (len == 0) {
            continue;
        }
        
        /* Check for quit */
        if (strncmp(line, "quit", 4) == 0 || strncmp(line, "exit", 4) == 0) {
            break;
        }
        
        /* Handle shorthand commands */
        if (strncmp(line, "buy ", 4) == 0) {
            /* Parse: buy SYMBOL PRICE QTY */
            char symbol[16];
            int price, qty;
            if (sscanf(line + 4, "%15s %d %d", symbol, &price, &qty) == 3) {
                snprintf(cleaned, sizeof(cleaned), "N,1,%s,%d,%d,B,%d\n",
                         symbol, price, qty, order_id++);
                fprintf(stderr, "→ %s", cleaned);
                send_framed_message(sockfd, cleaned, strlen(cleaned));
                continue;
            } else {
                fprintf(stderr, "Usage: buy SYMBOL PRICE QTY\n");
                continue;
            }
        }
        
        if (strncmp(line, "sell ", 5) == 0) {
            char symbol[16];
            int price, qty;
            if (sscanf(line + 5, "%15s %d %d", symbol, &price, &qty) == 3) {
                snprintf(cleaned, sizeof(cleaned), "N,1,%s,%d,%d,S,%d\n",
                         symbol, price, qty, order_id++);
                fprintf(stderr, "→ %s", cleaned);
                send_framed_message(sockfd, cleaned, strlen(cleaned));
                continue;
            } else {
                fprintf(stderr, "Usage: sell SYMBOL PRICE QTY\n");
                continue;
            }
        }
        
        if (strcmp(line, "flush") == 0 || strcmp(line, "F") == 0) {
            const char* flush = "F\n";
            fprintf(stderr, "→ F\n");
            send_framed_message(sockfd, flush, strlen(flush));
            continue;
        }
        
        /* Strip spaces from CSV and send */
        strip_csv_spaces(cleaned, line, sizeof(cleaned));
        
        /* Ensure newline */
        len = strlen(cleaned);
        if (len > 0 && cleaned[len-1] != '\n') {
            cleaned[len] = '\n';
            cleaned[len+1] = '\0';
            len++;
        }
        
        if (!send_framed_message(sockfd, cleaned, len)) {
            fprintf(stderr, "Failed to send message\n");
            break;
        }
        
        nanosleep(&ts, NULL);
    }
}

/* ============================================================================
 * Response Reader Thread
 * ============================================================================ */

static void* response_reader_thread(void* arg) {
    int sockfd = *(int*)arg;
    char buffer[MAX_MESSAGE_SIZE];
    size_t len;
    
    fprintf(stderr, "\n=== Server Responses ===\n");
    
    while (1) {
        if (!recv_framed_message(sockfd, buffer, sizeof(buffer), &len)) {
            fprintf(stderr, "\n[Connection closed or error]\n");
            break;
        }
        
        /* Print response with clear formatting */
        fprintf(stderr, "[RECV] ");
        fwrite(buffer, 1, len, stderr);
        if (len > 0 && buffer[len-1] != '\n') {
            fprintf(stderr, "\n");
        }
    }
    
    return NULL;
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <host> <port> [scenario]\n", prog);
    fprintf(stderr, "\nScenarios:\n");
    fprintf(stderr, "  1  - Simple orders (buy + sell + flush)\n");
    fprintf(stderr, "  2  - Matching trade\n");
    fprintf(stderr, "  3  - Cancel order\n");
    fprintf(stderr, "  i  - Interactive mode (default)\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s localhost 1234 1     # Run scenario 1\n", prog);
    fprintf(stderr, "  %s localhost 1234       # Interactive mode\n", prog);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char* host = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char* scenario = (argc > 3) ? argv[3] : "i";
    
    int sockfd = connect_to_server(host, port);
    if (sockfd < 0) {
        return 1;
    }
    
    /* Start response reader thread */
    pthread_t reader_thread;
    if (pthread_create(&reader_thread, NULL, response_reader_thread, &sockfd) != 0) {
        fprintf(stderr, "Failed to create reader thread\n");
        close(sockfd);
        return 1;
    }
    
    nanosleep(&ts, NULL);
    
    /* Run scenario */
    if (strcmp(scenario, "1") == 0) {
        run_scenario_1(sockfd);
    } else if (strcmp(scenario, "2") == 0) {
        run_scenario_2(sockfd);
    } else if (strcmp(scenario, "3") == 0) {
        run_scenario_3(sockfd);
    } else if (strcmp(scenario, "i") == 0 || strcmp(scenario, "interactive") == 0) {
        run_interactive_mode(sockfd);
    } else {
        fprintf(stderr, "Unknown scenario: %s\n", scenario);
        print_usage(argv[0]);
        close(sockfd);
        return 1;
    }
    
    sleep(1);
    
    fprintf(stderr, "\n=== Disconnecting ===\n");
    close(sockfd);
    
    sleep(1);
    
    return 0;
}

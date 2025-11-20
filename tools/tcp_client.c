#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define MAX_MESSAGE_SIZE 2048
#define FRAME_HEADER_SIZE 4

// Simple framing helpers
static bool send_framed_message(int sockfd, const char* msg, size_t msg_len) {
    // Build framed message: [4-byte length][message]
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

static bool recv_framed_message(int sockfd, char* msg_buffer, size_t* msg_len) {
    // Read 4-byte header
    uint32_t length_be;
    size_t received = 0;
    
    while (received < FRAME_HEADER_SIZE) {
        ssize_t n = read(sockfd, ((char*)&length_be) + received, 
                        FRAME_HEADER_SIZE - received);
        if (n <= 0) {
            return false;
        }
        received += n;
    }
    
    uint32_t length = ntohl(length_be);
    if (length > MAX_MESSAGE_SIZE) {
        fprintf(stderr, "Message too large: %u bytes\n", length);
        return false;
    }
    
    // Read message body
    received = 0;
    while (received < length) {
        ssize_t n = read(sockfd, msg_buffer + received, length - received);
        if (n <= 0) {
            return false;
        }
        received += n;
    }
    
    *msg_len = length;
    return true;
}

// Connect to server
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

// Test scenarios
static void run_scenario_1(int sockfd) {
    fprintf(stderr, "\n=== Scenario 1: Simple Orders ===\n");
    
    // Client 1 places buy order
    const char* order1 = "N, 1, IBM, 100, 50, B, 1\n";
    fprintf(stderr, "Sending: %s", order1);
    send_framed_message(sockfd, order1, strlen(order1));
    
    sleep(1);
    
    // Client 1 places sell order
    const char* order2 = "N, 1, IBM, 105, 50, S, 2\n";
    fprintf(stderr, "Sending: %s", order2);
    send_framed_message(sockfd, order2, strlen(order2));
    
    sleep(1);
    
    // Flush
    const char* flush = "F\n";
    fprintf(stderr, "Sending: FLUSH\n");
    send_framed_message(sockfd, flush, strlen(flush));
    
    sleep(1);
}

static void run_scenario_2(int sockfd) {
    fprintf(stderr, "\n=== Scenario 2: Matching Trade ===\n");
    
    // Client 1 places buy at 100
    const char* buy = "N, 1, IBM, 100, 50, B, 1\n";
    fprintf(stderr, "Sending BUY: %s", buy);
    send_framed_message(sockfd, buy, strlen(buy));
    
    sleep(1);
    
    // Client 1 places sell at 100 (should match)
    const char* sell = "N, 1, IBM, 100, 50, S, 2\n";
    fprintf(stderr, "Sending SELL: %s", sell);
    send_framed_message(sockfd, sell, strlen(sell));
    
    sleep(1);
}

static void run_scenario_3(int sockfd) {
    fprintf(stderr, "\n=== Scenario 3: Cancel Order ===\n");
    
    // Place order
    const char* order = "N, 1, IBM, 100, 50, B, 1\n";
    fprintf(stderr, "Sending: %s", order);
    send_framed_message(sockfd, order, strlen(order));
    
    sleep(1);
    
    // Cancel it
    const char* cancel = "C, 1, 1\n";
    fprintf(stderr, "Sending CANCEL: %s", cancel);
    send_framed_message(sockfd, cancel, strlen(cancel));
    
    sleep(1);
}

static void run_interactive_mode(int sockfd) {
    fprintf(stderr, "\n=== Interactive Mode ===\n");
    fprintf(stderr, "Enter orders (or 'quit' to exit):\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  N, 1, IBM, 100, 50, B, 1   (new buy order)\n");
    fprintf(stderr, "  N, 1, IBM, 100, 50, S, 2   (new sell order)\n");
    fprintf(stderr, "  C, 1, 1                     (cancel order 1)\n");
    fprintf(stderr, "  F                           (flush)\n\n");
    
    char line[1024];
    while (1) {
        fprintf(stderr, "> ");
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        
        // Remove newline if present
        size_t len = strlen(line);
        if (len > 0 && line[len-1] != '\n') {
            line[len] = '\n';
            line[len+1] = '\0';
            len++;
        }
        
        if (strncmp(line, "quit", 4) == 0 || strncmp(line, "exit", 4) == 0) {
            break;
        }
        
        if (!send_framed_message(sockfd, line, len)) {
            fprintf(stderr, "Failed to send message\n");
            break;
        }
        
        // Small delay to let server process
        usleep(100000);
    }
}

// Response reader thread (runs in background)
static void* response_reader_thread(void* arg) {
    int sockfd = *(int*)arg;
    char buffer[MAX_MESSAGE_SIZE];
    size_t len;
    
    fprintf(stderr, "\n=== Server Responses ===\n");
    
    while (1) {
        if (!recv_framed_message(sockfd, buffer, &len)) {
            fprintf(stderr, "\n[Connection closed or error]\n");
            break;
        }
        
        // Print response
        fwrite(buffer, 1, len, stderr);
        if (len > 0 && buffer[len-1] != '\n') {
            fprintf(stderr, "\n");
        }
    }
    
    return NULL;
}

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
    
    // Connect to server
    int sockfd = connect_to_server(host, port);
    if (sockfd < 0) {
        return 1;
    }
    
    // Start response reader thread
    pthread_t reader_thread;
    if (pthread_create(&reader_thread, NULL, response_reader_thread, &sockfd) != 0) {
        fprintf(stderr, "Failed to create reader thread\n");
        close(sockfd);
        return 1;
    }
    
    // Give reader thread time to start
    usleep(100000);
    
    // Run scenario
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
    
    // Give time for final responses
    sleep(1);
    
    fprintf(stderr, "\n=== Disconnecting ===\n");
    close(sockfd);
    
    // Wait a bit for reader thread to finish
    sleep(1);
    
    return 0;
}

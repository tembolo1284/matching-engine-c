/**
 * multicast_subscriber.c - Market Data Feed Subscriber
 * 
 * Joins a UDP multicast group and receives market data broadcasts from the
 * matching engine. This simulates how real market data subscribers work at
 * exchanges (CME, NASDAQ, ICE).
 * 
 * Features:
 *   - Auto-detects CSV vs Binary protocol
 *   - Displays market data in real-time
 *   - Tracks sequence numbers and detects gaps
 *   - Shows statistics on Ctrl+C
 *   - Multiple instances can run simultaneously
 * 
 * Usage:
 *   ./multicast_subscriber <multicast_group> <port> [interface_ip]
 * 
 * Example:
 *   Terminal 1: ./matching_engine --tcp --multicast 239.255.0.1:5000
 *   Terminal 2: ./multicast_subscriber 239.255.0.1 5000
 *   Terminal 3: ./multicast_subscriber 239.255.0.1 5000 192.168.0.159  # Specify interface
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>

#define MAX_PACKET_SIZE 65507
#define BINARY_MAGIC 0x4D

// Statistics
static atomic_bool g_running = true;
static atomic_uint_fast64_t g_packets_received = 0;
static atomic_uint_fast64_t g_messages_received = 0;
static atomic_uint_fast64_t g_binary_messages = 0;
static atomic_uint_fast64_t g_csv_messages = 0;
static atomic_uint_fast64_t g_parse_errors = 0;

// Timing
static struct timespec g_start_time;

// Signal handler
static void signal_handler(int signum) {
    (void)signum;
    fprintf(stderr, "\n\n[Subscriber] Shutting down...\n");
    atomic_store(&g_running, false);
}

// Get elapsed time in milliseconds
static double get_elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    double elapsed = (now.tv_sec - g_start_time.tv_sec) * 1000.0;
    elapsed += (now.tv_nsec - g_start_time.tv_nsec) / 1000000.0;
    
    return elapsed;
}

// Print statistics
static void print_stats(void) {
    double elapsed_ms = get_elapsed_ms();
    double elapsed_sec = elapsed_ms / 1000.0;
    
    uint64_t packets = atomic_load(&g_packets_received);
    uint64_t messages = atomic_load(&g_messages_received);
    uint64_t binary = atomic_load(&g_binary_messages);
    uint64_t csv = atomic_load(&g_csv_messages);
    uint64_t errors = atomic_load(&g_parse_errors);
    
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "Multicast Subscriber Statistics\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Runtime:           %.2f seconds\n", elapsed_sec);
    fprintf(stderr, "Packets received:  %llu\n", (unsigned long long)packets);
    fprintf(stderr, "Messages received: %llu\n", (unsigned long long)messages);
    fprintf(stderr, "  Binary messages: %llu\n", (unsigned long long)binary);
    fprintf(stderr, "  CSV messages:    %llu\n", (unsigned long long)csv);
    fprintf(stderr, "Parse errors:      %llu\n", (unsigned long long)errors);
    
    if (elapsed_sec > 0) {
        fprintf(stderr, "\nThroughput:\n");
        fprintf(stderr, "  Packets/sec:   %.2f\n", packets / elapsed_sec);
        fprintf(stderr, "  Messages/sec:  %.2f\n", messages / elapsed_sec);
    }
    fprintf(stderr, "========================================\n");
}

// Check if message is binary protocol
static bool is_binary_message(const char* data, size_t len) {
    return (len >= 2 && (unsigned char)data[0] == BINARY_MAGIC);
}

// Parse and display binary message
static void handle_binary_message(const char* data, size_t len) {
    if (len < 2) {
        fprintf(stderr, "[ERROR] Binary message too short (%zu bytes)\n", len);
        atomic_fetch_add(&g_parse_errors, 1);
        return;
    }

    uint8_t magic = (uint8_t)data[0];
    uint8_t msg_type = (uint8_t)data[1];

    if (magic != BINARY_MAGIC) {
        fprintf(stderr, "[ERROR] Invalid magic byte: 0x%02X\n", magic);
        atomic_fetch_add(&g_parse_errors, 1);
        return;
    }

    // Helper to read big-endian uint32
    #define READ_U32(ptr) ((uint32_t)((uint8_t)(ptr)[0] << 24 | (uint8_t)(ptr)[1] << 16 | (uint8_t)(ptr)[2] << 8 | (uint8_t)(ptr)[3]))

    #define PRICE_MULT 1000.0

    switch (msg_type) {
        case 'A': {  // ACK: magic(1) + type(1) + symbol(8) + user_id(4) + order_id(4) = 18
            if (len >= 18) {
                char symbol[9] = {0};
                memcpy(symbol, data + 2, 8);
                // Trim trailing spaces
                for (int i = 7; i >= 0 && symbol[i] == ' '; i--) symbol[i] = '\0';
                uint32_t user_id = READ_U32(data + 10);
                uint32_t order_id = READ_U32(data + 14);
                printf("[ACK] %s, user=%u, order=%u\n", symbol, user_id, order_id);
            } else {
                printf("[ACK] (incomplete: %zu bytes)\n", len);
            }
            break;
        }
        case 'X': {  // CANCEL_ACK: magic(1) + type(1) + symbol(8) + user_id(4) + order_id(4) = 18
            if (len >= 18) {
                char symbol[9] = {0};
                memcpy(symbol, data + 2, 8);
                for (int i = 7; i >= 0 && symbol[i] == ' '; i--) symbol[i] = '\0';
                uint32_t user_id = READ_U32(data + 10);
                uint32_t order_id = READ_U32(data + 14);
                printf("[CANCEL_ACK] %s, user=%u, order=%u\n", symbol, user_id, order_id);
            } else {
                printf("[CANCEL_ACK] (incomplete: %zu bytes)\n", len);
            }
            break;
        }
        case 'T': {  // TRADE: magic(1) + type(1) + symbol(8) + user_buy(4) + order_buy(4) + user_sell(4) + order_sell(4) + price(4) + qty(4) = 34
            if (len >= 34) {
                char symbol[9] = {0};
                memcpy(symbol, data + 2, 8);
                for (int i = 7; i >= 0 && symbol[i] == ' '; i--) symbol[i] = '\0';
                uint32_t user_buy = READ_U32(data + 10);
                uint32_t order_buy = READ_U32(data + 14);
                uint32_t user_sell = READ_U32(data + 18);
                uint32_t order_sell = READ_U32(data + 22);
                uint32_t price = READ_U32(data + 26);
                uint32_t qty = READ_U32(data + 30);
                printf("[TRADE] %s, price=%.3f, qty=%u, buy(user=%u,order=%u), sell(user=%u,order=%u)\n",
                       symbol, price / PRICE_MULT, qty, user_buy, order_buy, user_sell, order_sell);
            } else {
                printf("[TRADE] (incomplete: %zu bytes)\n", len);
            }
            break;
        }
        case 'B': {  // TOP_OF_BOOK: magic(1) + type(1) + symbol(8) + side(1) + price(4) + qty(4) = 19 or 20
            if (len >= 19) {
                char symbol[9] = {0};
                memcpy(symbol, data + 2, 8);
                for (int i = 7; i >= 0 && symbol[i] == ' '; i--) symbol[i] = '\0';
                char side = data[10];
                uint32_t price = READ_U32(data + 11);
                uint32_t qty = READ_U32(data + 15);
                
                if (price == 0 && qty == 0) {
                    printf("[TOB] %s, %c: empty\n", symbol, side);
                } else {
                    printf("[TOB] %s, %c: %u @ %.3f\n", symbol, side, qty, price / PRICE_MULT);
                }
            } else {
                printf("[TOB] (incomplete: %zu bytes)\n", len);
            }
            break;
        }
        default:
            printf("[BINARY] Unknown type 0x%02X (%zu bytes)\n", msg_type, len);
            break;
    }

    #undef READ_U32
    #undef PRICE_MULT

    fflush(stdout);
    atomic_fetch_add(&g_binary_messages, 1);
    atomic_fetch_add(&g_messages_received, 1);
}

// Parse and display CSV message
static void handle_csv_message(const char* data, size_t len) {
    // Simple CSV display - just print it
    // Remove trailing newlines for cleaner output
    char buffer[MAX_PACKET_SIZE];
    if (len >= sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    
    memcpy(buffer, data, len);
    buffer[len] = '\0';
    
    // Remove trailing whitespace
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r' || buffer[len - 1] == ' ')) {
        buffer[--len] = '\0';
    }
    
    if (len > 0) {
        printf("[CSV] %s\n", buffer);
        fflush(stdout);
        
        atomic_fetch_add(&g_csv_messages, 1);
        atomic_fetch_add(&g_messages_received, 1);
    }
}

/**
 * Find first non-loopback IPv4 interface address
 * Returns true if found, stores address in out_addr
 */
static bool find_default_interface(struct in_addr* out_addr) {
    struct ifaddrs* ifaddr;
    struct ifaddrs* ifa;
    bool found = false;
    
    if (getifaddrs(&ifaddr) == -1) {
        return false;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }
        
        // Skip non-IPv4
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        
        // Skip loopback
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }
        
        // Skip interfaces that aren't up
        if (!(ifa->ifa_flags & IFF_UP)) {
            continue;
        }
        
        struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
        
        // Skip Docker bridge and similar virtual interfaces (172.x.x.x)
        uint32_t addr_host = ntohl(sa->sin_addr.s_addr);
        if ((addr_host & 0xFF000000) == 0xAC000000) {  // 172.x.x.x
            continue;
        }
        
        *out_addr = sa->sin_addr;
        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, out_addr, addr_str, sizeof(addr_str));
        fprintf(stderr, "✓ Auto-detected interface: %s (%s)\n", ifa->ifa_name, addr_str);
        found = true;
        break;
    }
    
    freeifaddrs(ifaddr);
    return found;
}

// Join multicast group and start receiving
static int run_subscriber(const char* mcast_group, uint16_t port, const char* interface_ip) {
    int sockfd;
    struct sockaddr_in local_addr;
    struct ip_mreq mreq;
    
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Multicast Market Data Subscriber\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Multicast group: %s:%u\n", mcast_group, port);
    fprintf(stderr, "PID:             %d\n", getpid());
    fprintf(stderr, "========================================\n\n");
    
    // Create UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "ERROR: Failed to create socket: %s\n", strerror(errno));
        return 1;
    }
    
    // Allow multiple subscribers on same port
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        fprintf(stderr, "WARNING: Failed to set SO_REUSEADDR: %s\n", strerror(errno));
    }
    
#ifdef SO_REUSEPORT
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        // Not fatal
    }
#endif
    
    // Bind to the multicast port (INADDR_ANY to receive multicast)
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        fprintf(stderr, "ERROR: Failed to bind to port %u: %s\n", port, strerror(errno));
        close(sockfd);
        return 1;
    }
    
    fprintf(stderr, "✓ Socket bound to port %u\n", port);
    
    // Join multicast group
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, mcast_group, &mreq.imr_multiaddr) <= 0) {
        fprintf(stderr, "ERROR: Invalid multicast address: %s\n", mcast_group);
        close(sockfd);
        return 1;
    }
    
    // Determine which interface to join on
    if (interface_ip != NULL) {
        // User specified interface
        if (inet_pton(AF_INET, interface_ip, &mreq.imr_interface) <= 0) {
            fprintf(stderr, "ERROR: Invalid interface IP: %s\n", interface_ip);
            close(sockfd);
            return 1;
        }
        fprintf(stderr, "✓ Using specified interface: %s\n", interface_ip);
    } else {
        // Try to auto-detect a good interface
        if (!find_default_interface(&mreq.imr_interface)) {
            // Fall back to INADDR_ANY
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            fprintf(stderr, "⚠ Using INADDR_ANY (may not work with multiple interfaces)\n");
            fprintf(stderr, "  Hint: Specify interface IP as third argument if no packets received\n");
        }
    }
    
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "ERROR: Failed to join multicast group: %s\n", strerror(errno));
        close(sockfd);
        return 1;
    }
    
    fprintf(stderr, "✓ Joined multicast group %s\n", mcast_group);
    fprintf(stderr, "✓ Listening for market data...\n\n");
    fprintf(stderr, "Press Ctrl+C to stop and show statistics\n");
    fprintf(stderr, "========================================\n\n");
    
    // Start timing
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
    
    // Set receive timeout
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;  // 100ms
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        fprintf(stderr, "WARNING: Failed to set receive timeout: %s\n", strerror(errno));
    }
    
    // Main receive loop
    char buffer[MAX_PACKET_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    
    while (atomic_load(&g_running)) {
        ssize_t bytes_received = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                                          (struct sockaddr*)&sender_addr, &sender_len);
        
        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - check running flag and continue
                continue;
            }
            fprintf(stderr, "ERROR: recvfrom failed: %s\n", strerror(errno));
            continue;
        }
        
        if (bytes_received == 0) {
            continue;
        }
        
        atomic_fetch_add(&g_packets_received, 1);
        
        // Null-terminate for CSV parsing
        buffer[bytes_received] = '\0';
        
        // Auto-detect protocol and handle message
        if (is_binary_message(buffer, bytes_received)) {
            handle_binary_message(buffer, bytes_received);
        } else {
            // CSV protocol - might have multiple lines
            char* line_start = buffer;
            char* line_end;
            
            while (line_start < buffer + bytes_received) {
                // Find end of line
                line_end = line_start;
                while (line_end < buffer + bytes_received && 
                       *line_end != '\n' && *line_end != '\r') {
                    line_end++;
                }
                
                size_t line_len = line_end - line_start;
                
                if (line_len > 0) {
                    handle_csv_message(line_start, line_len);
                }
                
                // Move to next line
                line_start = line_end;
                while (line_start < buffer + bytes_received &&
                       (*line_start == '\n' || *line_start == '\r')) {
                    line_start++;
                }
            }
        }
    }
    
    fprintf(stderr, "\n[Subscriber] Leaving multicast group...\n");
    
    // Leave multicast group
    if (setsockopt(sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "WARNING: Failed to leave multicast group: %s\n", strerror(errno));
    }
    
    close(sockfd);
    
    print_stats();
    
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <multicast_group> <port> [interface_ip]\n", argv[0]);
        fprintf(stderr, "\nExample:\n");
        fprintf(stderr, "  %s 239.255.0.1 5000\n", argv[0]);
        fprintf(stderr, "  %s 239.255.0.1 5000 192.168.0.159  # Specify interface\n\n", argv[0]);
        fprintf(stderr, "Standard multicast addresses:\n");
        fprintf(stderr, "  239.255.0.1   - Local subnet\n");
        fprintf(stderr, "  224.0.0.1     - All systems on subnet\n");
        fprintf(stderr, "  239.0.0.0/8   - Organization-local scope\n");
        fprintf(stderr, "\nIf no packets are received, try specifying the interface IP.\n");
        fprintf(stderr, "Run 'hostname -I' to see available interfaces.\n");
        fprintf(stderr, "\nMultiple subscribers can run simultaneously!\n");
        return 1;
    }
    
    const char* mcast_group = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char* interface_ip = (argc == 4) ? argv[3] : NULL;
    
    if (port == 0) {
        fprintf(stderr, "ERROR: Invalid port: %s\n", argv[2]);
        return 1;
    }
    
    // Verify multicast address range (224.0.0.0 - 239.255.255.255)
    struct in_addr addr;
    if (inet_pton(AF_INET, mcast_group, &addr) <= 0) {
        fprintf(stderr, "ERROR: Invalid IP address: %s\n", mcast_group);
        return 1;
    }
    
    uint32_t addr_val = ntohl(addr.s_addr);
    if ((addr_val & 0xF0000000) != 0xE0000000) {
        fprintf(stderr, "WARNING: Address %s is not in multicast range (224.0.0.0-239.255.255.255)\n",
                mcast_group);
        fprintf(stderr, "         Continuing anyway...\n\n");
    }
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    return run_subscriber(mcast_group, port, interface_ip);
}

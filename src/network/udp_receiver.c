#include "network/udp_receiver.h"
#include "protocol/binary/binary_protocol.h"
#include "protocol/binary/binary_message_parser.h"
#include "protocol/symbol_router.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sched.h>

/**
 * Get symbol from input message for routing
 */
static const char* get_symbol_from_input_msg(const input_msg_t* msg) {
    switch (msg->type) {
        case INPUT_MSG_NEW_ORDER:
            return msg->data.new_order.symbol;
        case INPUT_MSG_CANCEL:
            return msg->data.cancel.symbol;
        case INPUT_MSG_FLUSH:
            return NULL;  // Flush has no symbol
        default:
            return NULL;
    }
}

/**
 * Route message to appropriate queue(s)
 * Returns true if message was successfully enqueued
 */
static bool route_message(udp_receiver_t* receiver, 
                          const input_msg_envelope_t* envelope,
                          const input_msg_t* msg) {
    
    if (receiver->num_output_queues == 1) {
        // Single processor mode - send to the only queue
        int retry_count = 0;
        const int MAX_RETRIES = 1000;
        
        while (!input_envelope_queue_enqueue(receiver->output_queues[0], envelope)) {
            retry_count++;
            if (retry_count >= MAX_RETRIES) {
                return false;
            }
            sched_yield();
        }
        atomic_fetch_add(&receiver->messages_to_processor[0], 1);
        return true;
    }
    
    // Dual processor mode - route by symbol
    const char* symbol = get_symbol_from_input_msg(msg);
    
    if (msg->type == INPUT_MSG_FLUSH || !symbol_is_valid(symbol)) {
        // Flush or cancel without symbol → send to BOTH queues
        bool success_0 = false;
        bool success_1 = false;
        
        int retry_count = 0;
        const int MAX_RETRIES = 1000;
        
        // Send to queue 0
        while (!success_0 && retry_count < MAX_RETRIES) {
            if (input_envelope_queue_enqueue(receiver->output_queues[0], envelope)) {
                success_0 = true;
                atomic_fetch_add(&receiver->messages_to_processor[0], 1);
            } else {
                retry_count++;
                sched_yield();
            }
        }
        
        // Send to queue 1
        retry_count = 0;
        while (!success_1 && retry_count < MAX_RETRIES) {
            if (input_envelope_queue_enqueue(receiver->output_queues[1], envelope)) {
                success_1 = true;
                atomic_fetch_add(&receiver->messages_to_processor[1], 1);
            } else {
                retry_count++;
                sched_yield();
            }
        }
        
        return success_0 && success_1;
    }
    
    // Route by symbol
    int processor_id = get_processor_id_for_symbol(symbol);
    
    int retry_count = 0;
    const int MAX_RETRIES = 1000;
    
    while (!input_envelope_queue_enqueue(receiver->output_queues[processor_id], envelope)) {
        retry_count++;
        if (retry_count >= MAX_RETRIES) {
            return false;
        }
        sched_yield();
    }
    
    atomic_fetch_add(&receiver->messages_to_processor[processor_id], 1);
    return true;
}

/**
 * Initialize UDP receiver (single processor mode)
 */
void udp_receiver_init(udp_receiver_t* receiver, 
                       input_envelope_queue_t* output_queue, 
                       uint16_t port) {
    memset(receiver, 0, sizeof(*receiver));
    
    receiver->output_queues[0] = output_queue;
    receiver->output_queues[1] = NULL;
    receiver->num_output_queues = 1;
    receiver->output_queue = output_queue;  // Backward compatibility
    
    receiver->port = port;
    receiver->sockfd = -1;
    
    message_parser_init(&receiver->csv_parser);
    binary_message_parser_init(&receiver->binary_parser);
    
    atomic_init(&receiver->running, false);
    atomic_init(&receiver->started, false);
    atomic_init(&receiver->packets_received, 0);
    atomic_init(&receiver->messages_parsed, 0);
    atomic_init(&receiver->messages_dropped, 0);
    atomic_init(&receiver->messages_to_processor[0], 0);
    atomic_init(&receiver->messages_to_processor[1], 0);
    atomic_init(&receiver->sequence, 0);
}

/**
 * Initialize UDP receiver for dual-processor mode
 */
void udp_receiver_init_dual(udp_receiver_t* receiver,
                            input_envelope_queue_t* output_queue_0,
                            input_envelope_queue_t* output_queue_1,
                            uint16_t port) {
    memset(receiver, 0, sizeof(*receiver));
    
    receiver->output_queues[0] = output_queue_0;
    receiver->output_queues[1] = output_queue_1;
    receiver->num_output_queues = 2;
    receiver->output_queue = output_queue_0;  // Backward compatibility
    
    receiver->port = port;
    receiver->sockfd = -1;
    
    message_parser_init(&receiver->csv_parser);
    binary_message_parser_init(&receiver->binary_parser);
    
    atomic_init(&receiver->running, false);
    atomic_init(&receiver->started, false);
    atomic_init(&receiver->packets_received, 0);
    atomic_init(&receiver->messages_parsed, 0);
    atomic_init(&receiver->messages_dropped, 0);
    atomic_init(&receiver->messages_to_processor[0], 0);
    atomic_init(&receiver->messages_to_processor[1], 0);
    atomic_init(&receiver->sequence, 0);
}

/**
 * Destroy UDP receiver and cleanup resources
 */
void udp_receiver_destroy(udp_receiver_t* receiver) {
    udp_receiver_stop(receiver);
    
    if (receiver->sockfd >= 0) {
        close(receiver->sockfd);
        receiver->sockfd = -1;
    }
}

/**
 * Setup UDP socket
 */
bool udp_receiver_setup_socket(udp_receiver_t* receiver) {
    // Create UDP socket - use IPv6 which also accepts IPv4 on most systems
    receiver->sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (receiver->sockfd < 0) {
        fprintf(stderr, "ERROR: Failed to create UDP socket: %s\n", strerror(errno));
        return false;
    }
    
    // Disable IPv6-only mode to accept both IPv4 and IPv6
    int no = 0;
    if (setsockopt(receiver->sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no)) < 0) {
        fprintf(stderr, "WARNING: Failed to disable IPV6_V6ONLY: %s\n", strerror(errno));
    }
    
    // Set socket to reuse address
    int reuse = 1;
    if (setsockopt(receiver->sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        fprintf(stderr, "WARNING: Failed to set SO_REUSEADDR: %s\n", strerror(errno));
    }
    
    // Set receive buffer size to 10MB (handle bursts)
    int buffer_size = UDP_RECV_BUFFER_SIZE;
    if (setsockopt(receiver->sockfd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
        fprintf(stderr, "WARNING: Failed to set receive buffer size: %s\n", strerror(errno));
    }
    
    // Verify buffer size
    socklen_t optlen = sizeof(buffer_size);
    if (getsockopt(receiver->sockfd, SOL_SOCKET, SO_RCVBUF, &buffer_size, &optlen) == 0) {
        fprintf(stderr, "UDP socket receive buffer size: %d bytes\n", buffer_size);
    }
    
    // Set receive timeout
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000; // 100ms
    if (setsockopt(receiver->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        fprintf(stderr, "WARNING: Failed to set receive timeout: %s\n", strerror(errno));
    }
    
    // Bind to port (IPv6 address that also accepts IPv4)
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;  // Listen on all interfaces (IPv4 and IPv6)
    addr.sin6_port = htons(receiver->port);
    
    if (bind(receiver->sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ERROR: Failed to bind to port %u: %s\n", receiver->port, strerror(errno));
        close(receiver->sockfd);
        receiver->sockfd = -1;
        return false;
    }
    
    fprintf(stderr, "UDP Receiver listening on port %u (IPv4 + IPv6)\n", receiver->port);
    return true;
}

/**
 * Get binary message size by type
 * Returns 0 if unknown type
 */
static size_t get_binary_message_size(uint8_t msg_type) {
    switch (msg_type) {
        case BINARY_MSG_NEW_ORDER:
            return sizeof(binary_new_order_t);
        case BINARY_MSG_CANCEL:
            return sizeof(binary_cancel_t);
        case BINARY_MSG_FLUSH:
            return sizeof(binary_flush_t);
        default:
            return 0;
    }
}

/**
 * Parse and route a single message (binary or CSV)
 * Returns number of bytes consumed, or 0 on error
 */
static size_t parse_and_route_message(udp_receiver_t* receiver,
                                      const char* data,
                                      size_t remaining) {
    input_msg_t msg;
    size_t consumed = 0;
    
    /* Check if this is a binary message */
    if (remaining >= 2 && (uint8_t)data[0] == BINARY_MAGIC) {
        uint8_t msg_type = (uint8_t)data[1];
        size_t msg_size = get_binary_message_size(msg_type);
        
        if (msg_size == 0) {
            fprintf(stderr, "Unknown binary message type: 0x%02X\n", msg_type);
            return 1;  /* Skip unknown byte */
        }
        
        if (remaining < msg_size) {
            fprintf(stderr, "Incomplete binary message: have %zu, need %zu\n", 
                    remaining, msg_size);
            return 0;  /* Can't process - shouldn't happen with UDP */
        }
        
        if (binary_message_parser_parse(&receiver->binary_parser, data, msg_size, &msg)) {
            uint64_t seq = atomic_fetch_add(&receiver->sequence, 1);
            input_msg_envelope_t envelope = create_input_envelope(&msg, 0, seq);
            
            if (route_message(receiver, &envelope, &msg)) {
                atomic_fetch_add_explicit(&receiver->messages_parsed, 1, 
                                          memory_order_relaxed);
            } else {
                fprintf(stderr, "WARNING: Input queue full, dropping message!\n");
                atomic_fetch_add(&receiver->messages_dropped, 1);
            }
        }
        
        return msg_size;
    }
    
    /* CSV protocol - find end of line */
    const char* line_end = data;
    while (line_end < data + remaining && *line_end != '\n' && *line_end != '\r') {
        line_end++;
    }
    
    size_t line_len = line_end - data;
    
    if (line_len > 0) {
        char line_buffer[MAX_INPUT_LINE_LENGTH];
        if (line_len < MAX_INPUT_LINE_LENGTH) {
            memcpy(line_buffer, data, line_len);
            line_buffer[line_len] = '\0';
            
            if (message_parser_parse(&receiver->csv_parser, line_buffer, &msg)) {
                uint64_t seq = atomic_fetch_add(&receiver->sequence, 1);
                input_msg_envelope_t envelope = create_input_envelope(&msg, 0, seq);
                
                if (route_message(receiver, &envelope, &msg)) {
                    atomic_fetch_add_explicit(&receiver->messages_parsed, 1, 
                                              memory_order_relaxed);
                } else {
                    fprintf(stderr, "WARNING: Input queue full, dropping message!\n");
                    atomic_fetch_add(&receiver->messages_dropped, 1);
                }
            }
        } else {
            fprintf(stderr, "ERROR: CSV line too long (%zu bytes)\n", line_len);
        }
        consumed = line_len;
    }
    
    /* Skip any trailing newline characters */
    const char* skip = data + consumed;
    while (skip < data + remaining && (*skip == '\n' || *skip == '\r')) {
        skip++;
        consumed++;
    }
    
    return consumed > 0 ? consumed : 1;  /* Always advance at least 1 byte */
}

/**
 * Thread entry point for UDP receiver
 */
void* udp_receiver_thread_func(void* arg) {
    udp_receiver_t* receiver = (udp_receiver_t*)arg;
    
    fprintf(stderr, "UDP Receiver thread started on port %d (mode: %s)\n", 
            receiver->port,
            receiver->num_output_queues == 2 ? "dual-processor" : "single-processor");
    
    char buffer[MAX_UDP_PACKET_SIZE];
    struct sockaddr_in6 client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    while (atomic_load_explicit(&receiver->running, memory_order_acquire)) {
        // Receive UDP packet
        ssize_t bytes_received = recvfrom(receiver->sockfd,
                                          buffer,
                                          MAX_UDP_PACKET_SIZE - 1,
                                          0,
                                          (struct sockaddr*)&client_addr,
                                          &client_len);
        
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
        
        // Null-terminate for CSV parsing safety
        buffer[bytes_received] = '\0';
        
        atomic_fetch_add_explicit(&receiver->packets_received, 1, memory_order_relaxed);
        
        /* Parse all messages in the packet */
        const char* ptr = buffer;
        size_t remaining = (size_t)bytes_received;
        
        while (remaining > 0) {
            size_t consumed = parse_and_route_message(receiver, ptr, remaining);
            if (consumed == 0) {
                break;  /* Incomplete message - shouldn't happen with UDP */
            }
            ptr += consumed;
            remaining -= consumed;
        }
    }
    
    fprintf(stderr, "UDP Receiver thread stopped.\n");
    udp_receiver_print_stats(receiver);
    
    return NULL;
}

/**
 * Start receiving (spawns thread)
 */
bool udp_receiver_start(udp_receiver_t* receiver) {
    // Check if already running
    bool expected = false;
    if (!atomic_compare_exchange_strong(&receiver->started, &expected, true)) {
        return false;  // Already started
    }
    
    // Setup socket BEFORE starting thread
    if (!udp_receiver_setup_socket(receiver)) {
        fprintf(stderr, "ERROR: Failed to setup UDP socket\n");
        atomic_store(&receiver->started, false);
        return false;
    }
    
    atomic_store_explicit(&receiver->running, true, memory_order_release);
    
    // Create thread
    int result = pthread_create(&receiver->thread, NULL, udp_receiver_thread_func, receiver);
    if (result != 0) {
        fprintf(stderr, "ERROR: Failed to create UDP receiver thread: %s\n", strerror(result));
        atomic_store(&receiver->running, false);
        atomic_store(&receiver->started, false);
        close(receiver->sockfd);
        receiver->sockfd = -1;
        return false;
    }
    
    return true;
}

/**
 * Stop receiving (signals thread to exit and waits)
 */
void udp_receiver_stop(udp_receiver_t* receiver) {
    // Check if started
    if (!atomic_load(&receiver->started)) {
        return;
    }
    
    // Signal thread to stop
    atomic_store_explicit(&receiver->running, false, memory_order_release);
    
    // Wait for thread to finish
    pthread_join(receiver->thread, NULL);
    
    atomic_store(&receiver->started, false);
}

/**
 * Check if thread is running
 */
bool udp_receiver_is_running(const udp_receiver_t* receiver) {
    return atomic_load_explicit(&receiver->running, memory_order_acquire);
}

/**
 * Get statistics
 */
uint64_t udp_receiver_get_packets_received(const udp_receiver_t* receiver) {
    return atomic_load(&receiver->packets_received);
}

uint64_t udp_receiver_get_messages_parsed(const udp_receiver_t* receiver) {
    return atomic_load(&receiver->messages_parsed);
}

uint64_t udp_receiver_get_messages_dropped(const udp_receiver_t* receiver) {
    return atomic_load(&receiver->messages_dropped);
}

/**
 * Print detailed statistics
 */
void udp_receiver_print_stats(const udp_receiver_t* receiver) {
    fprintf(stderr, "\n=== UDP Receiver Statistics ===\n");
    fprintf(stderr, "Packets received:      %llu\n",
            (unsigned long long)atomic_load(&receiver->packets_received));
    fprintf(stderr, "Messages parsed:       %llu\n",
            (unsigned long long)atomic_load(&receiver->messages_parsed));
    fprintf(stderr, "Messages dropped:      %llu\n",
            (unsigned long long)atomic_load(&receiver->messages_dropped));
    
    if (receiver->num_output_queues == 2) {
        fprintf(stderr, "Messages to Proc 0 (A-M): %llu\n",
                (unsigned long long)atomic_load(&receiver->messages_to_processor[0]));
        fprintf(stderr, "Messages to Proc 1 (N-Z): %llu\n",
                (unsigned long long)atomic_load(&receiver->messages_to_processor[1]));
    }
}

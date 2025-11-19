#include "network/udp_receiver.h"
#include "protocol/binary/binary_protocol.h"
#include "protocol/binary/binary_message_parser.h"
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
 * Initialize UDP receiver
 */
void udp_receiver_init(udp_receiver_t* receiver, input_queue_t* output_queue, uint16_t port) {
    receiver->output_queue = output_queue;
    receiver->port = port;
    receiver->sockfd = -1;
    
    message_parser_init(&receiver->csv_parser);
    
    memset(receiver->recv_buffer, 0, MAX_UDP_PACKET_SIZE);
    
    atomic_init(&receiver->running, false);
    atomic_init(&receiver->started, false);
    atomic_init(&receiver->packets_received, 0);
    atomic_init(&receiver->messages_parsed, 0);
    message_parser_init(&receiver->csv_parser);
    binary_message_parser_init(&receiver->binary_parser);
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
    // Create UDP socket
    receiver->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver->sockfd < 0) {
        fprintf(stderr, "ERROR: Failed to create UDP socket: %s\n", strerror(errno));
        return false;
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

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000; // 100ms
    if (setsockopt(receiver->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        fprintf(stderr, "WARNING: Failed to set receive timeout: %s\n", strerror(errno));
    }
    
    // Bind to port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(receiver->port);
    
    if (bind(receiver->sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ERROR: Failed to bind to port %u: %s\n", receiver->port, strerror(errno));
        close(receiver->sockfd);
        receiver->sockfd = -1;
        return false;
    }
    
    fprintf(stderr, "UDP Receiver listening on port %u\n", receiver->port);
    
    return true;
}

/**
 * Handle received UDP packet
 */
void udp_receiver_handle_packet(udp_receiver_t* receiver, const char* data, size_t length) {
    // CRITICAL: A single UDP packet may contain multiple lines (netcat behavior)
    // We need to parse each line separately
    
    char line_buffer[MAX_LINE_LENGTH];
    size_t line_start = 0;
    
    for (size_t i = 0; i < length; i++) {
        if (data[i] == '\n' || data[i] == '\r') {
            // Found end of line
            if (i > line_start) {
                // Copy line (skip the newline)
                size_t line_len = i - line_start;
                if (line_len >= MAX_LINE_LENGTH) {
                    line_len = MAX_LINE_LENGTH - 1;
                }
                
                memcpy(line_buffer, data + line_start, line_len);
                line_buffer[line_len] = '\0';
                
                // Parse the message
                input_msg_t msg;
                if (message_parser_parse(&receiver->csv_parser, line_buffer, &msg)) {
                    // Try to push to queue (non-blocking with retry)
                    int retry_count = 0;
                    const int MAX_RETRIES = 100;
                    
                    while (!input_queue_push(receiver->output_queue, &msg)) {
                        retry_count++;
                        if (retry_count >= MAX_RETRIES) {
                            fprintf(stderr, "WARNING: Input queue full, dropping message!\n");
                            break;
                        }
                        // Brief yield to give processor time to drain queue
                        sched_yield();
                    }
                    
                    if (retry_count < MAX_RETRIES) {
                        atomic_fetch_add_explicit(&receiver->messages_parsed, 1, memory_order_relaxed);
                    }
                }
            }
            
            // Skip any additional \r or \n characters
            while (i + 1 < length && (data[i + 1] == '\n' || data[i + 1] == '\r')) {
                i++;
            }
            
            line_start = i + 1;
        }
    }
    
    // Handle last line if it doesn't end with newline
    if (line_start < length) {
        size_t line_len = length - line_start;
        if (line_len >= MAX_LINE_LENGTH) {
            line_len = MAX_LINE_LENGTH - 1;
        }
        
        memcpy(line_buffer, data + line_start, line_len);
        line_buffer[line_len] = '\0';
        
        input_msg_t msg;
        if (message_parser_parse(&receiver->csv_parser, line_buffer, &msg)) {
            int retry_count = 0;
            const int MAX_RETRIES = 100;
            
            while (!input_queue_push(receiver->output_queue, &msg)) {
                retry_count++;
                if (retry_count >= MAX_RETRIES) {
                    fprintf(stderr, "WARNING: Input queue full, dropping message!\n");
                    break;
                }
                sched_yield();
            }
            
            if (retry_count < MAX_RETRIES) {
                atomic_fetch_add_explicit(&receiver->messages_parsed, 1, memory_order_relaxed);
            }
        }
    }
}

/**
 * Thread entry point for UDP receiver
 */
void* udp_receiver_thread_func(void* arg) {
    udp_receiver_t* receiver = (udp_receiver_t*)arg;

    fprintf(stderr, "UDP Receiver thread started on port %d\n", receiver->port);

    char buffer[MAX_UDP_PACKET_SIZE];
    struct sockaddr_in client_addr;
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

        // Null-terminate for CSV parsing
        buffer[bytes_received] = '\0';

        atomic_fetch_add_explicit(&receiver->packets_received, 1, memory_order_relaxed);

        // Split buffer by newlines for multi-line packets
        char* line_start = buffer;
        char* line_end;

        while (line_start < buffer + bytes_received) {
            // Find end of line
            line_end = line_start;
            while (line_end < buffer + bytes_received && *line_end != '\n' && *line_end != '\r') {
                line_end++;
            }

            // Calculate line length
            size_t line_len = line_end - line_start;

            if (line_len > 0) {
                input_msg_t msg;
                bool parse_success = false;

                /* Auto-detect message format */
                if (is_binary_message(line_start, line_len)) {
                    /* Binary protocol */
                    parse_success = binary_message_parser_parse(
                        &receiver->binary_parser,
                        line_start,
                        line_len,
                        &msg
                    );
                    if (parse_success) {
                        fprintf(stderr, "Parsed binary message (type 0x%02X, %zu bytes)\n", 
                                (unsigned char)line_start[1], line_len);
                    }
                } else {
                    /* CSV protocol - need to null-terminate this line */
                    char line_buffer[MAX_INPUT_LINE_LENGTH];
                    if (line_len < MAX_INPUT_LINE_LENGTH) {
                        memcpy(line_buffer, line_start, line_len);
                        line_buffer[line_len] = '\0';
                        
                        parse_success = message_parser_parse(
                            &receiver->csv_parser,
                            line_buffer,
                            &msg
                        );
                        if (parse_success) {
                            fprintf(stderr, "Parsed CSV message: %.*s\n", 
                                    (int)line_len, line_start);
                        }
                    } else {
                        fprintf(stderr, "ERROR: CSV line too long (%zu bytes)\n", line_len);
                    }
                }

                if (parse_success) {
                    // Push message to input queue with retry logic
                    int retry_count = 0;
                    const int MAX_RETRIES = 1000;

                    while (!input_queue_push(receiver->output_queue, &msg)) {
                        retry_count++;
                        if (retry_count >= MAX_RETRIES) {
                            fprintf(stderr, "WARNING: Input queue full, dropping message!\n");
                            atomic_fetch_add(&receiver->messages_dropped, 1);
                            break;
                        }
                        // Very brief wait
                        sched_yield();
                    }

                    if (retry_count < MAX_RETRIES) {
                        atomic_fetch_add_explicit(&receiver->messages_parsed, 1, memory_order_relaxed);
                    }
                } else {
                    fprintf(stderr, "ERROR: Failed to parse message\n");
                }
            }

            // Move to next line
            line_start = line_end;
            // Skip newline characters
            while (line_start < buffer + bytes_received && 
                   (*line_start == '\n' || *line_start == '\r')) {
                line_start++;
            }
        }
    }

    fprintf(stderr, "UDP Receiver thread stopped. Packets: %lu, Parsed: %lu, Dropped: %lu\n",
            atomic_load(&receiver->packets_received),
            atomic_load(&receiver->messages_parsed),
            atomic_load(&receiver->messages_dropped));

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

/**
 * Get messages dropped count
 */
uint64_t udp_receiver_get_messages_dropped(const udp_receiver_t* receiver) {
    return atomic_load(&receiver->messages_dropped);
}

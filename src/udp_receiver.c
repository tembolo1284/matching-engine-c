#include "udp_receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/**
 * Initialize UDP receiver
 */
void udp_receiver_init(udp_receiver_t* receiver, input_queue_t* output_queue, uint16_t port) {
    receiver->output_queue = output_queue;
    receiver->port = port;
    receiver->sockfd = -1;
    
    message_parser_init(&receiver->parser);
    
    memset(receiver->recv_buffer, 0, MAX_UDP_PACKET_SIZE);
    
    atomic_init(&receiver->running, false);
    atomic_init(&receiver->started, false);
    atomic_init(&receiver->packets_received, 0);
    atomic_init(&receiver->messages_parsed, 0);
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
                if (message_parser_parse(&receiver->parser, line_buffer, &msg)) {
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
        if (message_parser_parse(&receiver->parser, line_buffer, &msg)) {
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
 * Thread entry point
 */
void* udp_receiver_thread_func(void* arg) {
    udp_receiver_t* receiver = (udp_receiver_t*)arg;
    
    fprintf(stderr, "UDP Receiver thread started\n");
    
    // Setup socket
    if (!udp_receiver_setup_socket(receiver)) {
        atomic_store_explicit(&receiver->running, false, memory_order_release);
        return NULL;
    }
    
    // Main receive loop
    while (atomic_load_explicit(&receiver->running, memory_order_acquire)) {
        struct sockaddr_in remote_addr;
        socklen_t addr_len = sizeof(remote_addr);
        
        // Receive UDP packet (blocking with timeout)
        ssize_t bytes_received = recvfrom(receiver->sockfd, 
                                          receiver->recv_buffer, 
                                          MAX_UDP_PACKET_SIZE - 1,
                                          0,
                                          (struct sockaddr*)&remote_addr,
                                          &addr_len);
        
        if (bytes_received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                // Interrupted or would block - continue
                continue;
            }
            fprintf(stderr, "ERROR: recvfrom failed: %s\n", strerror(errno));
            break;
        }
        
        if (bytes_received > 0) {
            atomic_fetch_add_explicit(&receiver->packets_received, 1, memory_order_relaxed);
            
            // Null-terminate for safety
            receiver->recv_buffer[bytes_received] = '\0';
            
            // Handle the packet
            udp_receiver_handle_packet(receiver, receiver->recv_buffer, bytes_received);
        }
    }
    
    // Cleanup
    close(receiver->sockfd);
    receiver->sockfd = -1;
    
    fprintf(stderr, "UDP Receiver thread stopped. Packets: %lu, Messages: %lu\n",
            atomic_load(&receiver->packets_received),
            atomic_load(&receiver->messages_parsed));
    
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
    
    atomic_store_explicit(&receiver->running, true, memory_order_release);
    
    // Create thread
    int result = pthread_create(&receiver->thread, NULL, udp_receiver_thread_func, receiver);
    if (result != 0) {
        fprintf(stderr, "ERROR: Failed to create UDP receiver thread: %s\n", strerror(result));
        atomic_store(&receiver->running, false);
        atomic_store(&receiver->started, false);
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

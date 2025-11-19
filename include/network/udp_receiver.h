#ifndef MATCHING_ENGINE_UDP_RECEIVER_H
#define MATCHING_ENGINE_UDP_RECEIVER_H

#include "message_types.h"
#include "message_parser.h"
#include "binary_message_parser.h"
#include "queues.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * UdpReceiver - Thread 1: Receive UDP messages and parse them
 * 
 * Design decisions:
 * - Uses raw POSIX sockets (replaces boost::asio)
 * - Runs in separate pthread
 * - Parses incoming CSV messages
 * - Pushes parsed messages to lock-free queue
 * - Graceful shutdown via atomic flag
 * - Large receive buffer (10MB) to handle bursts
 */

#define MAX_UDP_PACKET_SIZE 65507
#define UDP_RECV_BUFFER_SIZE (10 * 1024 * 1024)  /* 10MB socket buffer */
#define MAX_INPUT_LINE_LENGTH 256

/**
 * UDP receiver state
 */
typedef struct {
    /* Output queue */
    input_queue_t* output_queue;
    
    /* Network configuration */
    uint16_t port;
    int sockfd;  /* Socket file descriptor */
    
    /* Receive buffer */
    char recv_buffer[MAX_UDP_PACKET_SIZE];
    
    /* Thread management */
    pthread_t thread;
    atomic_bool running;
    atomic_bool started;
    
    /* Statistics (optional) */
    atomic_uint_fast64_t packets_received;
    atomic_uint_fast64_t messages_parsed;
    atomic_uint_fast64_t messages_dropped;

    message_parser_t csv_parser;
    binary_message_parser_t binary_parser;

} udp_receiver_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize UDP receiver
 */
void udp_receiver_init(udp_receiver_t* receiver, input_queue_t* output_queue, uint16_t port);

/**
 * Destroy UDP receiver and cleanup resources
 */
void udp_receiver_destroy(udp_receiver_t* receiver);

/**
 * Start receiving (spawns thread)
 * Returns true on success, false on error
 */
bool udp_receiver_start(udp_receiver_t* receiver);

/**
 * Stop receiving (signals thread to exit and waits)
 */
void udp_receiver_stop(udp_receiver_t* receiver);

/**
 * Check if thread is running
 */
bool udp_receiver_is_running(const udp_receiver_t* receiver);

/**
 * Get statistics
 */
uint64_t udp_receiver_get_packets_received(const udp_receiver_t* receiver);
uint64_t udp_receiver_get_messages_parsed(const udp_receiver_t* receiver);

/* ============================================================================
 * Internal Functions (used by thread)
 * ============================================================================ */

/**
 * Thread entry point
 */
void* udp_receiver_thread_func(void* arg);

/**
 * Handle received UDP packet
 */
void udp_receiver_handle_packet(udp_receiver_t* receiver, const char* data, size_t length);

/**
 * Setup UDP socket
 */
bool udp_receiver_setup_socket(udp_receiver_t* receiver);

/**
 * Get messages dropped count
 */
uint64_t udp_receiver_get_messages_dropped(const udp_receiver_t* receiver);

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_UDP_RECEIVER_H */

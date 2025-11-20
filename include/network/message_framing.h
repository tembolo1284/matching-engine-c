#ifndef MESSAGE_FRAMING_H
#define MESSAGE_FRAMING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Message Framing Protocol for TCP Streams
 * 
 * Wire Format: [4-byte length (big-endian)][message payload]
 * 
 * Example:
 *   CSV message "N, 1, IBM, 100, 50, B, 1\n" (26 bytes)
 *   Wire: [0x00][0x00][0x00][0x1A]["N, 1, IBM, 100, 50, B, 1\n"]
 * 
 * This handles:
 *   - Partial reads (message split across TCP packets)
 *   - Partial writes (socket buffer full)
 *   - Both CSV and binary protocol messages
 */

// Maximum message size (16KB - reasonable for order messages)
#define MAX_FRAMED_MESSAGE_SIZE 16384

// Frame header size (4 bytes for length)
#define FRAME_HEADER_SIZE 4

/**
 * Frame a message for transmission
 * 
 * Takes a raw message and prepends it with a 4-byte length header (big-endian)
 * 
 * @param msg_data Raw message bytes
 * @param msg_len Length of raw message
 * @param output Output buffer (must be >= msg_len + 4)
 * @param output_len [OUT] Total bytes written to output
 * @return true on success, false if message too large
 */
bool frame_message(const char* msg_data, size_t msg_len,
                   char* output, size_t* output_len);

/**
 * Read state for incremental message reading
 * 
 * Maintains state across multiple read() calls to handle partial TCP reads
 */
typedef struct {
    char buffer[MAX_FRAMED_MESSAGE_SIZE];  // Read buffer
    size_t buffer_pos;                      // Current position in buffer
    uint32_t expected_length;               // Message length from header
    bool reading_header;                    // true = reading 4-byte header
                                            // false = reading message body
} framing_read_state_t;

/**
 * Initialize a read state
 */
void framing_read_state_init(framing_read_state_t* state);

/**
 * Process incoming bytes and extract complete messages
 * 
 * Call this with data from read(). It handles partial reads and returns
 * a complete message when available.
 * 
 * @param state Read state (maintains state across calls)
 * @param incoming_data Bytes received from socket
 * @param incoming_len Number of bytes received
 * @param msg_data [OUT] Pointer to complete message (if ready)
 * @param msg_len [OUT] Length of complete message
 * @return true if a complete message is ready, false if more data needed
 * 
 * Example usage:
 *   framing_read_state_t state;
 *   framing_read_state_init(&state);
 *   
 *   while (running) {
 *       ssize_t n = read(sock, buffer, sizeof(buffer));
 *       const char* msg;
 *       size_t msg_len;
 *       if (framing_read_process(&state, buffer, n, &msg, &msg_len)) {
 *           // Complete message ready - process it
 *           handle_message(msg, msg_len);
 *           framing_read_state_init(&state);  // Reset for next message
 *       }
 *   }
 */
bool framing_read_process(framing_read_state_t* state,
                          const char* incoming_data,
                          size_t incoming_len,
                          const char** msg_data,
                          size_t* msg_len);

/**
 * Write state for incremental message writing
 * 
 * Handles partial writes when socket buffer is full
 */
typedef struct {
    char buffer[MAX_FRAMED_MESSAGE_SIZE + FRAME_HEADER_SIZE];
    size_t total_len;      // Total bytes to write
    size_t written;        // Bytes already written
} framing_write_state_t;

/**
 * Initialize a write state with a framed message
 * 
 * @param state Write state
 * @param msg_data Raw message to send
 * @param msg_len Length of raw message
 * @return true on success, false if message too large
 */
bool framing_write_state_init(framing_write_state_t* state,
                               const char* msg_data,
                               size_t msg_len);

/**
 * Get pointer to data that still needs to be written
 * 
 * @param state Write state
 * @param data [OUT] Pointer to remaining data
 * @param len [OUT] Number of bytes remaining
 */
void framing_write_get_remaining(framing_write_state_t* state,
                                 const char** data,
                                 size_t* len);

/**
 * Mark bytes as written (after successful write() call)
 * 
 * @param state Write state
 * @param bytes_written Number of bytes successfully written
 */
void framing_write_mark_written(framing_write_state_t* state,
                                size_t bytes_written);

/**
 * Check if write is complete
 * 
 * @param state Write state
 * @return true if all bytes written
 */
bool framing_write_is_complete(framing_write_state_t* state);

#endif // MESSAGE_FRAMING_H

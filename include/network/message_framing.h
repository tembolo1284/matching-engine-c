#ifndef MESSAGE_FRAMING_H
#define MESSAGE_FRAMING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Message Framing for TCP Streams
 *
 * Handles length-prefixed message framing for reliable TCP communication.
 * Format: [4-byte big-endian length][payload]
 *
 * This solves the TCP stream reassembly problem - TCP provides a byte stream,
 * but we need discrete messages. Length-prefixing is the standard solution
 * used by most binary protocols (protobuf, FIX/FAST, etc.).
 *
 * Thread Safety: NOT thread-safe. Each connection needs its own framing state.
 *
 * Performance Notes:
 * - Zero-copy extraction via pointer return (valid until next extract)
 * - Minimizes memmove by processing multiple messages per read
 * - Buffer sizes tuned for typical trading message sizes
 *
 * Kernel Bypass Notes:
 * - Compatible with DPDK - operates on payload after L4 parsing
 * - For kernel bypass, framing operates on rte_mbuf data pointers
 * - State structures remain unchanged
 */

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Frame header size (4-byte length prefix, network byte order) */
#define FRAME_HEADER_SIZE 4

/* Maximum message size (excluding header) */
#define MAX_FRAMED_MESSAGE_SIZE 4096

/* Internal buffer size - holds header + max message + slack for partial reads */
#define FRAMING_BUFFER_SIZE (MAX_FRAMED_MESSAGE_SIZE + FRAME_HEADER_SIZE + 256)

/* Maximum messages to process per read call (Rule 2: bounded processing) */
#define MAX_MESSAGES_PER_READ 64

/* Compile-time validation */
_Static_assert(FRAMING_BUFFER_SIZE > MAX_FRAMED_MESSAGE_SIZE + FRAME_HEADER_SIZE,
               "Buffer must hold at least one max-size framed message");

/* ============================================================================
 * Result Codes
 * ============================================================================ */

/**
 * Result codes for framing operations
 */
typedef enum {
    FRAMING_OK = 0,             /* Operation succeeded */
    FRAMING_NEED_MORE_DATA,     /* Incomplete message, need more bytes */
    FRAMING_MESSAGE_READY,      /* Complete message available */
    FRAMING_ERROR               /* Protocol error (invalid length, etc.) */
} framing_result_t;

/* ============================================================================
 * Read-side State
 * ============================================================================ */

/**
 * Read-side framing state
 *
 * Accumulates bytes until a complete message is available.
 * The extract_buffer provides a stable copy of the message data
 * that remains valid until the next extract call.
 */
typedef struct {
    char buffer[FRAMING_BUFFER_SIZE];           /* Accumulation buffer */
    char extract_buffer[MAX_FRAMED_MESSAGE_SIZE]; /* Safe copy for extraction */
    size_t buffer_pos;                          /* Current write position */
    size_t expected_length;                     /* Expected payload length (0 if unknown) */
    bool reading_header;                        /* State machine flag */
} framing_read_state_t;

/* ============================================================================
 * Write-side State
 * ============================================================================ */

/**
 * Write-side framing state
 *
 * Handles partial writes for non-blocking sockets.
 * Tracks how much of the framed message has been sent.
 */
typedef struct {
    char buffer[FRAMING_BUFFER_SIZE];   /* Framed message (header + payload) */
    size_t total_len;                   /* Total bytes to send */
    size_t bytes_written;               /* Bytes sent so far */
} framing_write_state_t;

/* ============================================================================
 * Read-side API
 * ============================================================================ */

/**
 * Initialize read-side framing state
 *
 * Must be called before first use and after protocol errors.
 *
 * @param state Framing state to initialize
 *
 * Preconditions:
 * - state != NULL
 */
void framing_read_state_init(framing_read_state_t* state);

/**
 * Append received data to the framing buffer
 *
 * Call this with data received from recv()/read().
 *
 * @param state Framing state
 * @param data  Received data
 * @param len   Length of received data
 * @return Number of bytes consumed (may be less than len if buffer full)
 *
 * Preconditions:
 * - state != NULL
 * - data != NULL
 * - len > 0
 */
size_t framing_read_append(framing_read_state_t* state,
                           const char* data,
                           size_t len);

/**
 * Try to extract a complete message from the buffer
 *
 * Call this in a loop after append() until it returns NEED_MORE_DATA.
 *
 * @param state    Framing state
 * @param msg_data Output: pointer to message payload (valid until next extract)
 * @param msg_len  Output: length of message payload
 * @return FRAMING_MESSAGE_READY if message extracted,
 *         FRAMING_NEED_MORE_DATA if incomplete,
 *         FRAMING_ERROR on protocol error
 *
 * Preconditions:
 * - state != NULL
 * - msg_data != NULL
 * - msg_len != NULL
 *
 * Postconditions (on MESSAGE_READY):
 * - *msg_data points to valid message data
 * - *msg_len > 0 && *msg_len <= MAX_FRAMED_MESSAGE_SIZE
 */
framing_result_t framing_read_extract(framing_read_state_t* state,
                                       const char** msg_data,
                                       size_t* msg_len);

/**
 * Check if there's potentially more data to extract
 *
 * Useful for deciding whether to call extract() again.
 *
 * @param state Framing state
 * @return true if buffer may contain a complete message
 */
bool framing_read_has_data(const framing_read_state_t* state);

/**
 * Get number of buffered bytes
 *
 * @param state Framing state
 * @return Bytes currently in buffer
 */
static inline size_t framing_read_buffered(const framing_read_state_t* state) {
    return state ? state->buffer_pos : 0;
}

/* ============================================================================
 * Write-side API
 * ============================================================================ */

/**
 * Initialize write state with a message to send
 *
 * Frames the message with a length prefix.
 *
 * @param state   Write state to initialize
 * @param msg     Message payload to send
 * @param msg_len Length of message
 * @return true on success, false if message too large
 *
 * Preconditions:
 * - state != NULL
 * - msg != NULL
 * - msg_len > 0 && msg_len <= MAX_FRAMED_MESSAGE_SIZE
 */
bool framing_write_state_init(framing_write_state_t* state,
                              const char* msg,
                              size_t msg_len);

/**
 * Get pointer to remaining data to write
 *
 * Use this to get the data for send()/write().
 *
 * @param state Write state
 * @param data  Output: pointer to data to write
 * @param len   Output: length of remaining data
 *
 * Preconditions:
 * - state != NULL
 * - data != NULL
 * - len != NULL
 */
void framing_write_get_remaining(framing_write_state_t* state,
                                 const char** data,
                                 size_t* len);

/**
 * Mark bytes as successfully written
 *
 * Call this after send()/write() returns.
 *
 * @param state Write state
 * @param len   Number of bytes written
 *
 * Preconditions:
 * - state != NULL
 * - len <= remaining bytes
 */
void framing_write_mark_written(framing_write_state_t* state, size_t len);

/**
 * Check if all data has been written
 *
 * @param state Write state
 * @return true if complete message has been sent
 */
bool framing_write_is_complete(const framing_write_state_t* state);

/**
 * Get write progress
 *
 * @param state Write state
 * @return Bytes remaining to send
 */
static inline size_t framing_write_remaining(const framing_write_state_t* state) {
    return state ? (state->total_len - state->bytes_written) : 0;
}

/* ============================================================================
 * Simple Write API (for blocking sockets)
 * ============================================================================ */

/**
 * Frame a message with length prefix (simple version)
 *
 * For blocking sockets where you don't need partial write tracking.
 *
 * @param msg     Message to frame
 * @param msg_len Length of message
 * @param out     Output buffer (must be at least msg_len + FRAME_HEADER_SIZE)
 * @param out_len Output: total framed length
 * @return true on success, false if message too large
 *
 * Preconditions:
 * - msg != NULL
 * - out != NULL
 * - out_len != NULL
 * - msg_len <= MAX_FRAMED_MESSAGE_SIZE
 */
bool frame_message(const char* msg,
                   size_t msg_len,
                   char* out,
                   size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_FRAMING_H */

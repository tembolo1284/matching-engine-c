#ifndef MATCHING_ENGINE_MESSAGE_FRAMING_H
#define MATCHING_ENGINE_MESSAGE_FRAMING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Message Framing Layer
 * 
 * Handles TCP stream reassembly into discrete messages.
 * Uses length-prefixed framing: [4-byte length][payload]
 * 
 * Power of Ten Compliance:
 * - Rule 2: All extraction loops have fixed upper bounds (MAX_MESSAGES_PER_READ)
 * - Rule 3: No dynamic allocation - uses fixed-size buffers
 * - Rule 5: Assertions validate parameters and state
 */

/* Frame header is 4 bytes (network byte order length) */
#define FRAME_HEADER_SIZE 4

/* Maximum message payload size (excluding header) */
#define MAX_FRAMED_MESSAGE_SIZE 8192

/* Maximum complete messages to extract per read (Rule 2 compliance) */
#define MAX_MESSAGES_PER_READ 1024

/* Total buffer size including header space */
#define FRAMING_BUFFER_SIZE (MAX_FRAMED_MESSAGE_SIZE + FRAME_HEADER_SIZE)

/**
 * Read state for accumulating incoming TCP data
 */
typedef struct {
    char buffer[FRAMING_BUFFER_SIZE];
    size_t buffer_pos;          /* Current write position in buffer */
    uint32_t expected_length;   /* Expected message length (0 if reading header) */
    bool reading_header;        /* True if still reading length header */
} framing_read_state_t;

/**
 * Write state for sending framed messages
 */
typedef struct {
    char buffer[FRAMING_BUFFER_SIZE];
    size_t total_len;           /* Total bytes to send (header + payload) */
    size_t written;             /* Bytes already written */
} framing_write_state_t;

/**
 * Result of message extraction attempt
 */
typedef enum {
    FRAMING_NEED_MORE_DATA = 0,  /* Incomplete message, need more bytes */
    FRAMING_MESSAGE_READY = 1,   /* Complete message extracted */
    FRAMING_ERROR = -1           /* Protocol error (invalid length, overflow) */
} framing_result_t;

/* ============================================================================
 * Read-side API
 * ============================================================================ */

/**
 * Initialize read state
 * Must be called before first use
 */
void framing_read_state_init(framing_read_state_t* state);

/**
 * Append incoming data to the framing buffer
 * 
 * @param state     Framing state
 * @param data      Incoming data from recv()
 * @param len       Length of incoming data
 * @return          Number of bytes consumed (may be less than len if buffer full)
 */
size_t framing_read_append(framing_read_state_t* state, 
                           const char* data, 
                           size_t len);

/**
 * Try to extract a complete message from the buffer
 * 
 * Power of Ten Rule 2: This function extracts at most ONE message.
 * Caller must loop with a fixed upper bound (MAX_MESSAGES_PER_READ).
 * 
 * @param state     Framing state
 * @param msg_data  Output: pointer to message payload (valid until next call)
 * @param msg_len   Output: length of message payload
 * @return          FRAMING_MESSAGE_READY if message extracted,
 *                  FRAMING_NEED_MORE_DATA if incomplete,
 *                  FRAMING_ERROR on protocol violation
 */
framing_result_t framing_read_extract(framing_read_state_t* state,
                                       const char** msg_data,
                                       size_t* msg_len);

/**
 * Check if buffer has potential messages waiting
 * Used to decide whether to continue extraction loop
 */
bool framing_read_has_data(const framing_read_state_t* state);

/**
 * Get buffer statistics for debugging
 */
size_t framing_read_buffer_used(const framing_read_state_t* state);
size_t framing_read_buffer_available(const framing_read_state_t* state);

/* ============================================================================
 * Write-side API
 * ============================================================================ */

/**
 * Frame a message for sending (adds length header)
 * 
 * @param msg_data  Message payload
 * @param msg_len   Message payload length
 * @param output    Output buffer (must be at least msg_len + FRAME_HEADER_SIZE)
 * @param output_len Output: total framed message length
 * @return          true on success, false if message too large
 */
bool frame_message(const char* msg_data, size_t msg_len,
                   char* output, size_t* output_len);

/**
 * Initialize write state with a message to send
 */
bool framing_write_state_init(framing_write_state_t* state,
                               const char* msg_data,
                               size_t msg_len);

/**
 * Get pointer to remaining data to write
 */
void framing_write_get_remaining(framing_write_state_t* state,
                                  const char** data,
                                  size_t* len);

/**
 * Mark bytes as successfully written
 */
void framing_write_mark_written(framing_write_state_t* state,
                                 size_t bytes_written);

/**
 * Check if all data has been written
 */
bool framing_write_is_complete(framing_write_state_t* state);

/* ============================================================================
 * Legacy API (for backward compatibility)
 * ============================================================================ */

/**
 * @deprecated Use framing_read_append + framing_read_extract instead
 */
bool framing_read_process(framing_read_state_t* state,
                          const char* incoming_data,
                          size_t incoming_len,
                          const char** msg_data,
                          size_t* msg_len);

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_MESSAGE_FRAMING_H */

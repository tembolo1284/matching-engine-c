#ifndef MESSAGE_FRAMING_H
#define MESSAGE_FRAMING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Message Framing for TCP Streams
 * 
 * Handles length-prefixed message framing for reliable TCP communication.
 * Format: [4-byte big-endian length][payload]
 * 
 * Thread Safety: NOT thread-safe. Each connection needs its own framing state.
 */

/* Frame header size (4-byte length prefix) */
#define FRAME_HEADER_SIZE 4

/* Maximum message size (excluding header) */
#define MAX_FRAMED_MESSAGE_SIZE 4096

/* Internal buffer size - must hold at least header + max message + some extra */
#define FRAMING_BUFFER_SIZE (MAX_FRAMED_MESSAGE_SIZE + FRAME_HEADER_SIZE + 256)

/**
 * Result codes for framing operations
 */
typedef enum {
    FRAMING_OK = 0,
    FRAMING_NEED_MORE_DATA,
    FRAMING_MESSAGE_READY,
    FRAMING_ERROR
} framing_result_t;

/**
 * Read-side framing state
 * Accumulates bytes until a complete message is available
 */
typedef struct {
    char buffer[FRAMING_BUFFER_SIZE];
    char extract_buffer[MAX_FRAMED_MESSAGE_SIZE];  /* Safe copy of extracted message */
    size_t buffer_pos;
    size_t expected_length;
    bool reading_header;
} framing_read_state_t;

/**
 * Initialize read-side framing state
 */
void framing_read_state_init(framing_read_state_t* state);

/**
 * Append received data to the framing buffer
 * 
 * @param state  Framing state
 * @param data   Received data
 * @param len    Length of received data
 * @return       Number of bytes consumed (may be less than len if buffer full)
 */
size_t framing_read_append(framing_read_state_t* state, const char* data, size_t len);

/**
 * Try to extract a complete message from the buffer
 * 
 * @param state     Framing state
 * @param msg_data  Output: pointer to message payload (valid until next extract call)
 * @param msg_len   Output: length of message payload
 * @return          FRAMING_MESSAGE_READY if message extracted,
 *                  FRAMING_NEED_MORE_DATA if incomplete,
 *                  FRAMING_ERROR on protocol error
 */
framing_result_t framing_read_extract(framing_read_state_t* state,
                                       const char** msg_data,
                                       size_t* msg_len);

/**
 * Check if there's potentially more data to extract
 */
bool framing_read_has_data(const framing_read_state_t* state);

/**
 * Write-side: Frame a message with length prefix
 * 
 * @param msg       Message to frame
 * @param msg_len   Length of message
 * @param out       Output buffer (must be at least msg_len + FRAME_HEADER_SIZE)
 * @param out_len   Output: total framed length
 * @return          true on success
 */
bool frame_message(const char* msg, size_t msg_len, char* out, size_t* out_len);

#endif /* MESSAGE_FRAMING_H */

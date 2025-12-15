#include "network/message_framing.h"
#include <string.h>
#include <arpa/inet.h>  /* For htonl/ntohl */
#include <assert.h>

/**
 * Message Framing Implementation
 */

/* ============================================================================
 * Read-side Implementation
 * ============================================================================ */

void framing_read_state_init(framing_read_state_t* state) {
    assert(state != NULL && "NULL state in framing_read_state_init");
    
    state->buffer_pos = 0;
    state->expected_length = 0;
    state->reading_header = true;
}

size_t framing_read_append(framing_read_state_t* state, 
                           const char* data, 
                           size_t len) {
    assert(state != NULL && "NULL state in framing_read_append");
    assert(data != NULL && "NULL data in framing_read_append");
    assert(state->buffer_pos <= FRAMING_BUFFER_SIZE && "Buffer position overflow");
    
    size_t space_available = FRAMING_BUFFER_SIZE - state->buffer_pos;
    size_t to_copy = (len < space_available) ? len : space_available;
    
    if (to_copy > 0) {
        memcpy(state->buffer + state->buffer_pos, data, to_copy);
        state->buffer_pos += to_copy;
    }
    
    return to_copy;
}

framing_result_t framing_read_extract(framing_read_state_t* state,
                                       const char** msg_data,
                                       size_t* msg_len) {
    assert(state != NULL && "NULL state in framing_read_extract");
    assert(msg_data != NULL && "NULL msg_data in framing_read_extract");
    assert(msg_len != NULL && "NULL msg_len in framing_read_extract");
    assert(state->buffer_pos <= FRAMING_BUFFER_SIZE && "Buffer position overflow");
    
    /* Need at least header bytes */
    if (state->buffer_pos < FRAME_HEADER_SIZE) {
        return FRAMING_NEED_MORE_DATA;
    }
    
    /* Parse length header (big-endian, network byte order) */
    uint32_t length_be;
    memcpy(&length_be, state->buffer, FRAME_HEADER_SIZE);
    uint32_t payload_length = ntohl(length_be);
    
    /* Validate length - Rule 5: assertion for anomalous conditions */
    if (payload_length == 0) {
        /* Zero-length message is protocol error */
        framing_read_state_init(state);
        return FRAMING_ERROR;
    }
    
    if (payload_length > MAX_FRAMED_MESSAGE_SIZE) {
        /* Message too large - protocol error or attack */
        framing_read_state_init(state);
        return FRAMING_ERROR;
    }
    
    /* Calculate total message size (header + payload) */
    size_t total_message_size = FRAME_HEADER_SIZE + payload_length;
    
    /* Check if we have the complete message */
    if (state->buffer_pos < total_message_size) {
        return FRAMING_NEED_MORE_DATA;
    }
    
    /*
     * CRITICAL: Copy message to extract_buffer BEFORE memmove!
     * The memmove will shift remaining data to the front of buffer,
     * which would overwrite the message we're trying to return.
     */
    memcpy(state->extract_buffer, state->buffer + FRAME_HEADER_SIZE, payload_length);
    
    /* Now shift remaining data to front of buffer */
    size_t remaining = state->buffer_pos - total_message_size;
    
    if (remaining > 0) {
        /* Rule 2: memmove is O(n) bounded by buffer size */
        memmove(state->buffer, state->buffer + total_message_size, remaining);
    }
    
    state->buffer_pos = remaining;
    
    /* Return pointer to the safely copied data */
    *msg_data = state->extract_buffer;
    *msg_len = payload_length;
    
    /* Post-condition assertion */
    assert(state->buffer_pos <= FRAMING_BUFFER_SIZE && "Buffer corruption after extract");
    
    return FRAMING_MESSAGE_READY;
}

bool framing_read_has_data(const framing_read_state_t* state) {
    assert(state != NULL && "NULL state in framing_read_has_data");
    
    /* Check if we might have a complete message buffered */
    if (state->buffer_pos < FRAME_HEADER_SIZE) {
        return false;
    }
    
    /* Parse the length to see if we have enough data */
    uint32_t length_be;
    memcpy(&length_be, state->buffer, FRAME_HEADER_SIZE);
    uint32_t payload_length = ntohl(length_be);
    
    /* Sanity check */
    if (payload_length == 0 || payload_length > MAX_FRAMED_MESSAGE_SIZE) {
        return true;  /* Will trigger error on extract */
    }
    
    return state->buffer_pos >= (FRAME_HEADER_SIZE + payload_length);
}

/* ============================================================================
 * Write-side Implementation
 * ============================================================================ */

bool framing_write_state_init(framing_write_state_t* state, const char* msg, size_t msg_len) {
    assert(state != NULL && "NULL state in framing_write_state_init");
    assert(msg != NULL && "NULL msg in framing_write_state_init");
    
    if (msg_len > MAX_FRAMED_MESSAGE_SIZE) {
        return false;
    }
    
    /* Write length prefix in network byte order */
    uint32_t length_be = htonl((uint32_t)msg_len);
    memcpy(state->buffer, &length_be, FRAME_HEADER_SIZE);
    
    /* Copy payload */
    memcpy(state->buffer + FRAME_HEADER_SIZE, msg, msg_len);
    
    state->total_len = FRAME_HEADER_SIZE + msg_len;
    state->bytes_written = 0;
    
    return true;
}

void framing_write_get_remaining(framing_write_state_t* state, const char** data, size_t* len) {
    assert(state != NULL && "NULL state in framing_write_get_remaining");
    assert(data != NULL && "NULL data in framing_write_get_remaining");
    assert(len != NULL && "NULL len in framing_write_get_remaining");
    
    *data = state->buffer + state->bytes_written;
    *len = state->total_len - state->bytes_written;
}

void framing_write_mark_written(framing_write_state_t* state, size_t len) {
    assert(state != NULL && "NULL state in framing_write_mark_written");
    assert(state->bytes_written + len <= state->total_len && "Write overflow");
    
    state->bytes_written += len;
}

bool framing_write_is_complete(const framing_write_state_t* state) {
    assert(state != NULL && "NULL state in framing_write_is_complete");
    
    return state->bytes_written >= state->total_len;
}

/* ============================================================================
 * Simple Write API
 * ============================================================================ */

bool frame_message(const char* msg, size_t msg_len, char* out, size_t* out_len) {
    assert(msg != NULL && "NULL msg in frame_message");
    assert(out != NULL && "NULL out in frame_message");
    assert(out_len != NULL && "NULL out_len in frame_message");
    
    if (msg_len > MAX_FRAMED_MESSAGE_SIZE) {
        return false;
    }
    
    /* Write length prefix in network byte order */
    uint32_t length_be = htonl((uint32_t)msg_len);
    memcpy(out, &length_be, FRAME_HEADER_SIZE);
    
    /* Copy payload */
    memcpy(out + FRAME_HEADER_SIZE, msg, msg_len);
    
    *out_len = FRAME_HEADER_SIZE + msg_len;
    return true;
}

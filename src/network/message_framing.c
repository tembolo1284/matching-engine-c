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
    
    /* Complete message ready - return pointer to payload */
    *msg_data = state->buffer + FRAME_HEADER_SIZE;
    *msg_len = payload_length;
    
    /* Shift remaining data to front of buffer */
    size_t remaining = state->buffer_pos - total_message_size;
    
    if (remaining > 0) {
        /* Rule 2: memmove is O(n) bounded by buffer size */
        memmove(state->buffer, state->buffer + total_message_size, remaining);
    }
    
    state->buffer_pos = remaining;
    
    /* Post-condition assertion */
    assert(state->buffer_pos <= FRAMING_BUFFER_SIZE && "Buffer corruption after extract");
    
    return FRAMING_MESSAGE_READY;
}

bool framing_read_has_data(const framing_read_state_t* state) {
    assert(state != NULL && "NULL state in framing_read_has_data");
    return state->buffer_pos >= FRAME_HEADER_SIZE;
}

size_t framing_read_buffer_used(const framing_read_state_t* state) {
    assert(state != NULL && "NULL state in framing_read_buffer_used");
    return state->buffer_pos;
}

size_t framing_read_buffer_available(const framing_read_state_t* state) {
    assert(state != NULL && "NULL state in framing_read_buffer_available");
    return FRAMING_BUFFER_SIZE - state->buffer_pos;
}

/* ============================================================================
 * Write-side Implementation
 * ============================================================================ */

bool frame_message(const char* msg_data, size_t msg_len,
                   char* output, size_t* output_len) {
    assert(msg_data != NULL && "NULL msg_data in frame_message");
    assert(output != NULL && "NULL output in frame_message");
    assert(output_len != NULL && "NULL output_len in frame_message");
    
    /* Validate message size */
    if (msg_len > MAX_FRAMED_MESSAGE_SIZE) {
        return false;
    }
    
    /* Write 4-byte length header (big-endian) */
    uint32_t length_be = htonl((uint32_t)msg_len);
    memcpy(output, &length_be, FRAME_HEADER_SIZE);
    
    /* Write message payload */
    memcpy(output + FRAME_HEADER_SIZE, msg_data, msg_len);
    
    *output_len = FRAME_HEADER_SIZE + msg_len;
    
    return true;
}

bool framing_write_state_init(framing_write_state_t* state,
                               const char* msg_data,
                               size_t msg_len) {
    assert(state != NULL && "NULL state in framing_write_state_init");
    assert(msg_data != NULL && "NULL msg_data in framing_write_state_init");
    
    /* Validate size */
    if (msg_len > MAX_FRAMED_MESSAGE_SIZE) {
        return false;
    }
    
    /* Frame the message into buffer */
    size_t framed_len;
    if (!frame_message(msg_data, msg_len, state->buffer, &framed_len)) {
        return false;
    }
    
    state->total_len = framed_len;
    state->written = 0;
    
    return true;
}

void framing_write_get_remaining(framing_write_state_t* state,
                                  const char** data,
                                  size_t* len) {
    assert(state != NULL && "NULL state in framing_write_get_remaining");
    assert(data != NULL && "NULL data in framing_write_get_remaining");
    assert(len != NULL && "NULL len in framing_write_get_remaining");
    assert(state->written <= state->total_len && "Write position overflow");
    
    *data = state->buffer + state->written;
    *len = state->total_len - state->written;
}

void framing_write_mark_written(framing_write_state_t* state,
                                 size_t bytes_written) {
    assert(state != NULL && "NULL state in framing_write_mark_written");
    
    state->written += bytes_written;
    
    /* Clamp to prevent overflow */
    if (state->written > state->total_len) {
        state->written = state->total_len;
    }
}

bool framing_write_is_complete(framing_write_state_t* state) {
    assert(state != NULL && "NULL state in framing_write_is_complete");
    return state->written >= state->total_len;
}

/* ============================================================================
 * Legacy API (backward compatibility)
 * ============================================================================ */

bool framing_read_process(framing_read_state_t* state,
                          const char* incoming_data,
                          size_t incoming_len,
                          const char** msg_data,
                          size_t* msg_len) {
    assert(state != NULL && "NULL state in framing_read_process");
    assert(incoming_data != NULL && "NULL data in framing_read_process");
    assert(msg_data != NULL && "NULL msg_data in framing_read_process");
    assert(msg_len != NULL && "NULL msg_len in framing_read_process");
    
    /* Append new data */
    size_t consumed = framing_read_append(state, incoming_data, incoming_len);
    
    if (consumed < incoming_len) {
        /* Buffer overflow - some data lost */
        /* Note: This is the legacy behavior limitation */
    }
    
    /* Try to extract one message */
    framing_result_t result = framing_read_extract(state, msg_data, msg_len);
    
    return (result == FRAMING_MESSAGE_READY);
}

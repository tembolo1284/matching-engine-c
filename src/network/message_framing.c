#include "network/message_framing.h"
#include <string.h>
#include <arpa/inet.h>  // For htonl/ntohl

bool frame_message(const char* msg_data, size_t msg_len,
                   char* output, size_t* output_len) {
    // Validate message size
    if (msg_len > MAX_FRAMED_MESSAGE_SIZE) {
        return false;
    }
    
    // Write 4-byte length header (big-endian)
    uint32_t length_be = htonl((uint32_t)msg_len);
    memcpy(output, &length_be, FRAME_HEADER_SIZE);
    
    // Write message payload
    memcpy(output + FRAME_HEADER_SIZE, msg_data, msg_len);
    
    *output_len = FRAME_HEADER_SIZE + msg_len;
    return true;
}

void framing_read_state_init(framing_read_state_t* state) {
    state->buffer_pos = 0;
    state->expected_length = 0;
    state->reading_header = true;
}

bool framing_read_process(framing_read_state_t* state,
                          const char* incoming_data,
                          size_t incoming_len,
                          const char** msg_data,
                          size_t* msg_len) {
    // Copy incoming data to buffer
    size_t space_available = MAX_FRAMED_MESSAGE_SIZE - state->buffer_pos;
    size_t to_copy = (incoming_len < space_available) ? incoming_len : space_available;
    
    if (to_copy == 0) {
        // Buffer overflow - reset state
        framing_read_state_init(state);
        return false;
    }
    
    memcpy(state->buffer + state->buffer_pos, incoming_data, to_copy);
    state->buffer_pos += to_copy;
    
    // State machine: reading header or body
    if (state->reading_header) {
        // Need 4 bytes for header
        if (state->buffer_pos < FRAME_HEADER_SIZE) {
            return false;  // Need more data
        }
        
        // Parse length header (big-endian)
        uint32_t length_be;
        memcpy(&length_be, state->buffer, FRAME_HEADER_SIZE);
        state->expected_length = ntohl(length_be);
        
        // Validate length
        if (state->expected_length == 0 || 
            state->expected_length > MAX_FRAMED_MESSAGE_SIZE) {
            // Invalid length - reset
            framing_read_state_init(state);
            return false;
        }
        
        // Move to body reading state
        state->reading_header = false;
        
        // Shift buffer contents (remove header)
        memmove(state->buffer, state->buffer + FRAME_HEADER_SIZE,
                state->buffer_pos - FRAME_HEADER_SIZE);
        state->buffer_pos -= FRAME_HEADER_SIZE;
    }
    
    // Reading message body
    if (state->buffer_pos < state->expected_length) {
        return false;  // Need more data
    }
    
    // Complete message ready!
    *msg_data = state->buffer;
    *msg_len = state->expected_length;
    return true;
}

bool framing_write_state_init(framing_write_state_t* state,
                               const char* msg_data,
                               size_t msg_len) {
    // Validate size
    if (msg_len > MAX_FRAMED_MESSAGE_SIZE) {
        return false;
    }
    
    // Frame the message into buffer
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
    *data = state->buffer + state->written;
    *len = state->total_len - state->written;
}

void framing_write_mark_written(framing_write_state_t* state,
                                size_t bytes_written) {
    state->written += bytes_written;
    if (state->written > state->total_len) {
        state->written = state->total_len;
    }
}

bool framing_write_is_complete(framing_write_state_t* state) {
    return state->written >= state->total_len;
}

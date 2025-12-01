/**
 * engine_client.h - High-level matching engine client API
 *
 * Combines transport and codec layers into a unified interface for:
 *   - Connecting to the matching engine (auto-detect transport/encoding)
 *   - Sending orders, cancels, and flushes
 *   - Receiving and parsing responses
 *   - Optional multicast subscription for market data
 *
 * This is the primary API that scenarios and interactive mode use.
 */

#ifndef CLIENT_ENGINE_CLIENT_H
#define CLIENT_ENGINE_CLIENT_H

#include "client/client_config.h"
#include "client/transport.h"
#include "client/codec.h"
#include "protocol/message_types.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Constants
 * ============================================================ */

#define ENGINE_CLIENT_MAX_PENDING_RESPONSES 64

/* ============================================================
 * Callback Types
 * ============================================================ */

/**
 * Callback for received responses
 * 
 * @param msg       Decoded output message
 * @param user_data User-provided context
 */
typedef void (*response_callback_t)(const output_msg_t* msg, void* user_data);

/**
 * Callback for multicast market data
 * 
 * @param msg       Decoded output message
 * @param user_data User-provided context
 */
typedef void (*multicast_callback_t)(const output_msg_t* msg, void* user_data);

/* ============================================================
 * Engine Client Handle
 * ============================================================ */

/**
 * Engine client state
 */
typedef struct {
    /* Configuration */
    client_config_t     config;
    
    /* Transport layer */
    transport_t         transport;
    
    /* Codec layer */
    codec_t             codec;
    
    /* Multicast receiver (optional) */
    multicast_receiver_t    multicast;
    bool                    multicast_active;
    
    /* Callbacks */
    response_callback_t     response_callback;
    void*                   response_user_data;
    multicast_callback_t    multicast_callback;
    void*                   multicast_user_data;
    
    /* State */
    bool                connected;
    uint32_t            next_order_id;      /* Auto-incrementing order ID */
    
    /* Statistics */
    uint64_t            orders_sent;
    uint64_t            cancels_sent;
    uint64_t            flushes_sent;
    uint64_t            responses_received;
    uint64_t            multicast_received;
    
    /* Timing (nanoseconds) */
    uint64_t            last_send_time;
    uint64_t            last_recv_time;
    uint64_t            total_latency;      /* Sum of round-trip times */
    uint64_t            latency_samples;
    uint64_t            min_latency;
    uint64_t            max_latency;
    
} engine_client_t;

/* ============================================================
 * Lifecycle API
 * ============================================================ */

/**
 * Initialize engine client with configuration
 * 
 * @param client    Client handle
 * @param config    Configuration (copied)
 */
void engine_client_init(engine_client_t* client, const client_config_t* config);

/**
 * Connect to the matching engine
 * 
 * Performs auto-detection if configured:
 *   1. Try TCP connect (with timeout)
 *   2. Fall back to UDP if TCP fails
 *   3. Send probe order to detect encoding
 * 
 * @param client    Client handle
 * @return          true on success
 */
bool engine_client_connect(engine_client_t* client);

/**
 * Disconnect from the matching engine
 * 
 * @param client    Client handle
 */
void engine_client_disconnect(engine_client_t* client);

/**
 * Cleanup and destroy client
 * 
 * @param client    Client handle
 */
void engine_client_destroy(engine_client_t* client);

/**
 * Check if connected
 */
bool engine_client_is_connected(const engine_client_t* client);

/* ============================================================
 * Multicast API
 * ============================================================ */

/**
 * Join multicast group for market data
 * 
 * @param client    Client handle
 * @param group     Multicast group (e.g., "239.255.0.1")
 * @param port      Multicast port
 * @return          true on success
 */
bool engine_client_join_multicast(engine_client_t* client,
                                  const char* group,
                                  uint16_t port);

/**
 * Leave multicast group
 */
void engine_client_leave_multicast(engine_client_t* client);

/* ============================================================
 * Callback Registration
 * ============================================================ */

/**
 * Set callback for TCP/UDP responses
 */
void engine_client_set_response_callback(engine_client_t* client,
                                         response_callback_t callback,
                                         void* user_data);

/**
 * Set callback for multicast market data
 */
void engine_client_set_multicast_callback(engine_client_t* client,
                                          multicast_callback_t callback,
                                          void* user_data);

/* ============================================================
 * Order Entry API
 * ============================================================ */

/**
 * Send a new order
 * 
 * @param client    Client handle
 * @param symbol    Trading symbol
 * @param price     Price (0 for market order)
 * @param quantity  Order quantity
 * @param side      SIDE_BUY or SIDE_SELL
 * @param order_id  Client order ID (0 = auto-assign)
 * @return          Assigned order ID, or 0 on failure
 */
uint32_t engine_client_send_order(engine_client_t* client,
                                  const char* symbol,
                                  uint32_t price,
                                  uint32_t quantity,
                                  side_t side,
                                  uint32_t order_id);

/**
 * Send a cancel request
 * 
 * @param client    Client handle
 * @param order_id  Order ID to cancel
 * @return          true on success
 */
bool engine_client_send_cancel(engine_client_t* client, uint32_t order_id);

/**
 * Send a flush (cancel all orders)
 * 
 * @param client    Client handle
 * @return          true on success
 */
bool engine_client_send_flush(engine_client_t* client);

/* ============================================================
 * Response Handling API
 * ============================================================ */

/**
 * Poll for responses (non-blocking)
 * 
 * Checks both TCP/UDP and multicast (if active).
 * Invokes registered callbacks for each message.
 * 
 * @param client    Client handle
 * @return          Number of messages processed
 */
int engine_client_poll(engine_client_t* client);

/**
 * Receive a single response (blocking with timeout)
 * 
 * @param client    Client handle
 * @param msg       Output: received message
 * @param timeout_ms Timeout in milliseconds (-1 = block forever)
 * @return          true if message received
 */
bool engine_client_recv(engine_client_t* client,
                        output_msg_t* msg,
                        int timeout_ms);

/**
 * Receive all pending responses
 * 
 * Blocks until no more responses available (with short timeout).
 * Invokes response callback for each message.
 * 
 * @param client    Client handle
 * @param timeout_ms Timeout per message
 * @return          Number of messages received
 */
int engine_client_recv_all(engine_client_t* client, int timeout_ms);

/**
 * Wait for a specific response type
 * 
 * @param client    Client handle
 * @param type      Expected message type
 * @param msg       Output: received message
 * @param timeout_ms Timeout in milliseconds
 * @return          true if expected message received
 */
bool engine_client_wait_for(engine_client_t* client,
                            output_msg_type_t type,
                            output_msg_t* msg,
                            int timeout_ms);

/* ============================================================
 * Utility API
 * ============================================================ */

/**
 * Get detected transport type
 */
transport_type_t engine_client_get_transport(const engine_client_t* client);

/**
 * Get detected encoding type
 */
encoding_type_t engine_client_get_encoding(const engine_client_t* client);

/**
 * Get next auto-assigned order ID (without sending)
 */
uint32_t engine_client_peek_next_order_id(const engine_client_t* client);

/**
 * Reset order ID counter
 */
void engine_client_reset_order_id(engine_client_t* client, uint32_t start_id);

/**
 * Print client statistics
 */
void engine_client_print_stats(const engine_client_t* client);

/**
 * Reset statistics
 */
void engine_client_reset_stats(engine_client_t* client);

/* ============================================================
 * Timing Utilities
 * ============================================================ */

/**
 * Get current time in nanoseconds (monotonic clock)
 */
uint64_t engine_client_now_ns(void);

/**
 * Get average round-trip latency in nanoseconds
 */
uint64_t engine_client_get_avg_latency_ns(const engine_client_t* client);

/**
 * Get minimum latency in nanoseconds
 */
uint64_t engine_client_get_min_latency_ns(const engine_client_t* client);

/**
 * Get maximum latency in nanoseconds
 */
uint64_t engine_client_get_max_latency_ns(const engine_client_t* client);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_ENGINE_CLIENT_H */

#ifndef UNIFIED_MODE_H
#define UNIFIED_MODE_H

/**
 * Unified Server Mode - All transports running simultaneously
 * 
 * This is the primary server mode that starts:
 *   - TCP listener on port 1234
 *   - UDP receiver on port 1235
 *   - Multicast publisher on port 1236 (always binary)
 * 
 * All transports feed into the same dual-processor matching engine.
 * Output routing is handled automatically:
 *   - Ack/Cancel Ack/Reject → originating client only
 *   - Trade → both buyer and seller
 *   - Top of Book → all connected clients
 *   - Multicast → all messages in binary
 * 
 * Per-client protocol detection (CSV vs Binary) is automatic.
 */

#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

/**
 * Default ports for unified mode
 */
#define UNIFIED_TCP_PORT      1234
#define UNIFIED_UDP_PORT      1235
#define UNIFIED_MULTICAST_PORT 1236

/**
 * Default multicast group
 */
#define UNIFIED_MULTICAST_GROUP "239.255.0.1"

/**
 * Unified mode configuration
 */
typedef struct {
    /* Ports (use defaults if 0) */
    uint16_t tcp_port;
    uint16_t udp_port;
    uint16_t multicast_port;
    
    /* Multicast group (use default if NULL) */
    const char* multicast_group;
    
    /* Processing mode */
    bool single_processor;    /* false = dual processor (default) */
    
    /* Output options */
    bool quiet_mode;          /* Suppress per-message output for benchmarks */
    bool binary_default;      /* Default format for new clients (auto-detect overrides) */
    
    /* Optional: Disable specific transports (for testing) */
    bool disable_tcp;
    bool disable_udp;
    bool disable_multicast;
} unified_config_t;

/**
 * Initialize config with defaults
 */
static inline void unified_config_init(unified_config_t* config) {
    config->tcp_port = UNIFIED_TCP_PORT;
    config->udp_port = UNIFIED_UDP_PORT;
    config->multicast_port = UNIFIED_MULTICAST_PORT;
    config->multicast_group = UNIFIED_MULTICAST_GROUP;
    config->single_processor = false;
    config->quiet_mode = false;
    config->binary_default = false;
    config->disable_tcp = false;
    config->disable_udp = false;
    config->disable_multicast = false;
}

/* ============================================================================
 * Server Lifecycle
 * ============================================================================ */

/**
 * Run the unified server
 * 
 * This function blocks until shutdown is signaled (e.g., Ctrl+C).
 * 
 * Architecture:
 *   - TCP Listener Thread: Accepts connections, spawns per-client handlers
 *   - UDP Receiver Thread: Receives datagrams, tracks clients
 *   - Processor Thread(s): 1 or 2 depending on config
 *   - Output Router Thread: Routes outputs to correct clients + multicast
 * 
 * @param config  Server configuration
 * @return        0 on clean shutdown, non-zero on error
 */
int run_unified_server(const unified_config_t* config);

/* ============================================================================
 * Output Message Routing
 * ============================================================================ */

/**
 * Output routing decisions based on message type
 * 
 * OUTPUT_MSG_ACK:
 *   → Send to originating client (client_id in envelope)
 *   → Multicast
 * 
 * OUTPUT_MSG_CANCEL_ACK:
 *   → Send to originating client
 *   → Multicast
 * 
 * OUTPUT_MSG_REJECT:
 *   → Send to originating client
 *   → Multicast
 * 
 * OUTPUT_MSG_TRADE:
 *   → Send to buyer (buyer_user_id → lookup client_id)
 *   → Send to seller (seller_user_id → lookup client_id)
 *   → Multicast
 * 
 * OUTPUT_MSG_TOP_OF_BOOK:
 *   → Broadcast to ALL connected clients
 *   → Multicast
 * 
 * Note: For trades, we need a user_id → client_id mapping.
 * This is established when processing orders (user_id comes from client).
 */

/**
 * User-to-client mapping
 * 
 * When a client sends an order with user_id=X, we record that
 * user_id X belongs to client_id Y. This allows routing trade
 * confirmations to the correct client.
 * 
 * Note: In a real system, user_id would be authenticated and
 * one user could have multiple clients. For simplicity, we
 * assume 1:1 mapping (last client to use a user_id "owns" it).
 */

#define MAX_USER_ID_MAPPINGS 65536

/**
 * User ID to Client ID mapping entry
 */
typedef struct {
    uint32_t user_id;
    uint32_t client_id;
    bool active;
} user_client_mapping_t;

/**
 * User ID mapping table
 */
typedef struct {
    user_client_mapping_t mappings[MAX_USER_ID_MAPPINGS];
    pthread_rwlock_t lock;
} user_client_map_t;

/**
 * Initialize user-client map
 */
void user_client_map_init(user_client_map_t* map);

/**
 * Destroy user-client map
 */
void user_client_map_destroy(user_client_map_t* map);

/**
 * Set mapping: user_id → client_id
 * Called when processing an order from a client.
 */
void user_client_map_set(user_client_map_t* map, uint32_t user_id, uint32_t client_id);

/**
 * Get client_id for a user_id
 * Returns 0 if not found.
 */
uint32_t user_client_map_get(user_client_map_t* map, uint32_t user_id);

#endif /* UNIFIED_MODE_H */

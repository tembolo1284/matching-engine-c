/**
 * client_config.h - Configuration for matching engine client
 *
 * Defines configuration structures and defaults for the client.
 * Transport and encoding are auto-detected by default.
 * 
 * Reuses types from:
 *   - protocol/message_types.h (side_t, input_msg_t, output_msg_t)
 *   - protocol/binary/binary_protocol.h (wire format)
 */

#ifndef CLIENT_CONFIG_H
#define CLIENT_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Constants
 * ============================================================ */

#define CLIENT_DEFAULT_PORT         1234
#define CLIENT_DEFAULT_TIMEOUT_MS   1000
#define CLIENT_PROBE_TIMEOUT_MS     500
#define CLIENT_MAX_HOST_LEN         256
#define CLIENT_RECV_BUFFER_SIZE     8192
#define CLIENT_SEND_BUFFER_SIZE     8192

/* Multicast defaults */
#define CLIENT_DEFAULT_MCAST_GROUP  "239.255.0.1"
#define CLIENT_DEFAULT_MCAST_PORT   5000

/* ============================================================
 * Enums
 * ============================================================ */

/**
 * Transport type - auto-detected or forced
 */
typedef enum {
    TRANSPORT_AUTO = 0,     /* Try TCP first, fall back to UDP */
    TRANSPORT_TCP,          /* Force TCP */
    TRANSPORT_UDP           /* Force UDP */
} transport_type_t;

/**
 * Encoding type - auto-detected or forced
 */
typedef enum {
    ENCODING_AUTO = 0,      /* Probe server to detect */
    ENCODING_BINARY,        /* Force binary protocol */
    ENCODING_CSV            /* Force CSV protocol */
} encoding_type_t;

/**
 * Client operating mode
 */
typedef enum {
    MODE_INTERACTIVE = 0,   /* REPL mode - read commands from stdin */
    MODE_SCENARIO,          /* Run a predefined scenario */
    MODE_MULTICAST_ONLY     /* Only subscribe to multicast feed */
} client_mode_t;

/**
 * Connection state
 */
typedef enum {
    CONN_STATE_DISCONNECTED = 0,
    CONN_STATE_CONNECTING,
    CONN_STATE_CONNECTED,
    CONN_STATE_ERROR
} conn_state_t;

/* ============================================================
 * Configuration Structure
 * ============================================================ */

/**
 * Multicast configuration
 */
typedef struct {
    bool        enabled;
    char        group[64];
    uint16_t    port;
    int         sock_fd;        /* Populated after join */
} multicast_config_t;

/**
 * Client configuration
 */
typedef struct {
    /* Connection target */
    char                host[CLIENT_MAX_HOST_LEN];
    uint16_t            port;

    /* Transport and encoding (auto-detected if AUTO) */
    transport_type_t    transport;
    encoding_type_t     encoding;

    /* Detected values (populated during probe) */
    transport_type_t    detected_transport;
    encoding_type_t     detected_encoding;

    /* Operating mode */
    client_mode_t       mode;
    int                 scenario_id;        /* For MODE_SCENARIO */
    bool                fire_and_forget;    /* Don't wait for responses */
    bool                danger_burst;       /* No throttling in stress tests */

    /* Multicast subscription */
    multicast_config_t  multicast;

    /* Timeouts (milliseconds) */
    uint32_t            connect_timeout_ms;
    uint32_t            recv_timeout_ms;

    /* Verbosity */
    bool                verbose;
    bool                quiet;              /* Suppress non-essential output */

    /* User ID for orders (default 1) */
    uint32_t            user_id;
} client_config_t;

/* ============================================================
 * Default Configuration
 * ============================================================ */

/**
 * Initialize configuration with defaults
 */
static inline void client_config_init(client_config_t* config) {
    memset(config, 0, sizeof(*config));
    
    config->host[0] = '\0';
    config->port = CLIENT_DEFAULT_PORT;

    config->transport = TRANSPORT_AUTO;
    config->encoding = ENCODING_AUTO;
    config->detected_transport = TRANSPORT_AUTO;
    config->detected_encoding = ENCODING_AUTO;

    config->mode = MODE_INTERACTIVE;
    config->scenario_id = 0;
    config->fire_and_forget = false;
    config->danger_burst = false;

    config->multicast.enabled = false;
    config->multicast.group[0] = '\0';
    config->multicast.port = CLIENT_DEFAULT_MCAST_PORT;
    config->multicast.sock_fd = -1;

    config->connect_timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;
    config->recv_timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;

    config->verbose = false;
    config->quiet = false;

    config->user_id = 1;
}

/**
 * Get string representation of transport type
 */
static inline const char* transport_type_str(transport_type_t t) {
    switch (t) {
        case TRANSPORT_AUTO: return "auto";
        case TRANSPORT_TCP:  return "TCP";
        case TRANSPORT_UDP:  return "UDP";
        default:             return "unknown";
    }
}

/**
 * Get string representation of encoding type
 */
static inline const char* encoding_type_str(encoding_type_t e) {
    switch (e) {
        case ENCODING_AUTO:   return "auto";
        case ENCODING_BINARY: return "binary";
        case ENCODING_CSV:    return "CSV";
        default:              return "unknown";
    }
}

/**
 * Get string representation of client mode
 */
static inline const char* client_mode_str(client_mode_t m) {
    switch (m) {
        case MODE_INTERACTIVE:    return "interactive";
        case MODE_SCENARIO:       return "scenario";
        case MODE_MULTICAST_ONLY: return "multicast-only";
        default:                  return "unknown";
    }
}

/**
 * Validate configuration
 * Returns true if valid, false otherwise
 */
static inline bool client_config_validate(const client_config_t* config) {
    /* Must have host unless multicast-only mode */
    if (config->mode != MODE_MULTICAST_ONLY && config->host[0] == '\0') {
        return false;
    }
    
    /* Multicast-only mode requires multicast enabled */
    if (config->mode == MODE_MULTICAST_ONLY && !config->multicast.enabled) {
        return false;
    }
    
    /* Port must be non-zero */
    if (config->port == 0 && config->mode != MODE_MULTICAST_ONLY) {
        return false;
    }
    
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_CONFIG_H */

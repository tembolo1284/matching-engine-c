#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>

#include "modes/run_modes.h"
#include "modes/tcp_mode.h"
#include "modes/udp_mode.h"

// Global shutdown flag (used by all run modes)
atomic_bool g_shutdown = false;

// Signal handler for graceful shutdown
static void signal_handler(int signum) {
    fprintf(stderr, "\nReceived signal %d, shutting down gracefully...\n", signum);
    atomic_store(&g_shutdown, true);
}

// Usage information
static void print_usage(const char* program_name) {
    fprintf(stderr, "Usage: %s [options]\n", program_name);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --tcp [port]             Use TCP mode (default port: 1234)\n");
    fprintf(stderr, "  --udp [port]             Use UDP mode (default port: 1234)\n");
    fprintf(stderr, "  --binary                 Use binary protocol for output\n");
    fprintf(stderr, "  --quiet, --benchmark     Suppress per-message output (stats only)\n");
    fprintf(stderr, "  --dual-processor         Use dual-processor mode (A-M / N-Z) [DEFAULT]\n");
    fprintf(stderr, "  --single-processor       Use single-processor mode\n");
    fprintf(stderr, "  --multicast <group:port> Broadcast market data to multicast group\n");
    fprintf(stderr, "  --help                   Display this help message\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s --tcp                           # TCP, dual-processor (default)\n", program_name);
    fprintf(stderr, "  %s --tcp --single-processor        # TCP, single-processor\n", program_name);
    fprintf(stderr, "  %s --tcp 5000                      # TCP on port 5000, dual-processor\n", program_name);
    fprintf(stderr, "  %s --tcp --binary                  # TCP with binary output\n", program_name);
    fprintf(stderr, "  %s --tcp --multicast 239.255.0.1:5000  # TCP + multicast feed\n", program_name);
    fprintf(stderr, "  %s --udp                           # UDP, dual-processor\n", program_name);
    fprintf(stderr, "  %s --udp --quiet                   # UDP benchmark mode (no output)\n", program_name);
    fprintf(stderr, "  %s --udp --single-processor        # UDP, single-processor (legacy)\n", program_name);
    fprintf(stderr, "\nBenchmark Mode (--quiet):\n");
    fprintf(stderr, "  Suppresses per-message stdout output for throughput testing\n");
    fprintf(stderr, "  Statistics still printed to stderr on shutdown (Ctrl+C)\n");
    fprintf(stderr, "\nDual-Processor Mode:\n");
    fprintf(stderr, "  Symbols A-M → Processor 0 (e.g., AAPL, IBM, GOOGL, META)\n");
    fprintf(stderr, "  Symbols N-Z → Processor 1 (e.g., NVDA, TSLA, UBER, ZM)\n");
    fprintf(stderr, "  Provides ~2x throughput with parallel matching\n");
    fprintf(stderr, "\nMulticast Market Data:\n");
    fprintf(stderr, "  Broadcasts trades, TOB updates, acks to UDP multicast group\n");
    fprintf(stderr, "  Multiple subscribers receive simultaneously (like real exchanges!)\n");
    fprintf(stderr, "  Example subscribers: ./multicast_subscriber 239.255.0.1 5000\n");
}

// Parse multicast address (format: "239.255.0.1:5000")
static bool parse_multicast_address(const char* addr_str,
                                    char* group,
                                    size_t group_size,
                                    uint16_t* port) {
    char buffer[128];

    // Safe copy into local buffer, always null-terminated
    snprintf(buffer, sizeof(buffer), "%s", addr_str);

    char* colon = strchr(buffer, ':');
    if (!colon) {
        fprintf(stderr, "ERROR: Invalid multicast address format (expected group:port)\n");
        return false;
    }

    *colon = '\0';

    // Safe bounded copy of group part into caller buffer
    if (group_size == 0) {
        fprintf(stderr, "ERROR: multicast group buffer too small\n");
        return false;
    }
    size_t len = strnlen(buffer, group_size - 1);
    memcpy(group, buffer, len);
    group[len] = '\0';

    *port = (uint16_t)atoi(colon + 1);
    if (*port == 0) {
        fprintf(stderr, "ERROR: Invalid multicast port\n");
        return false;
    }

    return true;
}

// Parse command line arguments
static bool parse_args(int argc, char** argv, app_config_t* config) {
    // Defaults
    config->tcp_mode = true;        // Default to TCP
    config->port = 1234;
    config->binary_output = false;
    config->dual_processor = true;  // Default to dual-processor
    config->quiet_mode = false;     // Default to verbose output
    config->enable_multicast = false;
    config->multicast_group[0] = '\0';
    config->multicast_port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tcp") == 0) {
            config->tcp_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->port = (uint16_t)atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "--udp") == 0) {
            config->tcp_mode = false;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->port = (uint16_t)atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "--binary") == 0) {
            config->binary_output = true;
        } else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "--benchmark") == 0) {
            config->quiet_mode = true;
        } else if (strcmp(argv[i], "--dual-processor") == 0) {
            config->dual_processor = true;
        } else if (strcmp(argv[i], "--single-processor") == 0) {
            config->dual_processor = false;
        } else if (strcmp(argv[i], "--multicast") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ERROR: --multicast requires address (e.g., 239.255.0.1:5000)\n");
                return false;
            }
            config->enable_multicast = true;
            if (!parse_multicast_address(argv[i + 1],
                                         config->multicast_group,
                                         sizeof(config->multicast_group),
                                         &config->multicast_port)) {
                return false;
            }
            i++;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return false;
        } else if (argv[i][0] != '-') {
            config->port = (uint16_t)atoi(argv[i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return false;
        }
    }

    // Validate multicast only works in TCP mode (for now)
    if (config->enable_multicast && !config->tcp_mode) {
        fprintf(stderr, "WARNING: Multicast currently only supported in TCP mode\n");
        fprintf(stderr, "         Disabling multicast for UDP mode\n");
        config->enable_multicast = false;
    }

    return true;
}

// Main entry point
int main(int argc, char** argv) {
    // Parse configuration
    app_config_t config;
    if (!parse_args(argc, argv, &config)) {
        return 1;
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Dispatch to appropriate run mode
    int result;
    if (config.tcp_mode) {
        if (config.dual_processor) {
            result = run_tcp_dual_processor(&config);
        } else {
            result = run_tcp_single_processor(&config);
        }
    } else {
        if (config.dual_processor) {
            result = run_udp_dual_processor(&config);
        } else {
            result = run_udp_single_processor(&config);
        }
    }

    return result;
}

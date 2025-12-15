#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "modes/unified_mode.h"

/* ============================================================================
 * Global Shutdown Flag
 * ============================================================================ */

atomic_bool g_shutdown = false;

/* ============================================================================
 * Signal Handler
 * ============================================================================ */

static void signal_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n[SIGNAL] Caught signal %d, initiating shutdown...\n", sig);
    atomic_store(&g_shutdown, true);
}

static void setup_signal_handlers(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    /* Ignore SIGPIPE (broken pipe from TCP clients) */
    signal(SIGPIPE, SIG_IGN);
}

/* ============================================================================
 * Usage / Help
 * ============================================================================ */

static void print_usage(const char* program) {
    fprintf(stderr, "\n");
    fprintf(stderr, "Matching Engine - Unified Server\n");
    fprintf(stderr, "================================\n\n");
    fprintf(stderr, "Usage: %s [OPTIONS]\n\n", program);
    fprintf(stderr, "The server always starts with all transports:\n");
    fprintf(stderr, "  - TCP on port %d\n", UNIFIED_TCP_PORT);
    fprintf(stderr, "  - UDP on port %d\n", UNIFIED_UDP_PORT);
    fprintf(stderr, "  - Multicast on %s:%d (always binary)\n\n", 
            UNIFIED_MULTICAST_GROUP, UNIFIED_MULTICAST_PORT);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --binary           Use binary protocol as default\n");
    fprintf(stderr, "                     (per-client auto-detection still works)\n");
    fprintf(stderr, "  --quiet            Suppress per-message output (benchmark mode)\n");
    fprintf(stderr, "  --single-processor Use single processor instead of dual (A-M/N-Z)\n");
    fprintf(stderr, "  --no-tcp           Disable TCP listener\n");
    fprintf(stderr, "  --no-udp           Disable UDP receiver\n");
    fprintf(stderr, "  --no-multicast     Disable multicast publisher\n");
    fprintf(stderr, "  --help, -h         Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s                      # Start with defaults (CSV, dual processor)\n", program);
    fprintf(stderr, "  %s --binary             # Start with binary as default format\n", program);
    fprintf(stderr, "  %s --quiet              # Benchmark mode (stats only)\n", program);
    fprintf(stderr, "  %s --quiet --binary     # Binary benchmark mode\n", program);
    fprintf(stderr, "  %s --single-processor   # Use single processor\n", program);
    fprintf(stderr, "\n");
    fprintf(stderr, "Client connections:\n");
    fprintf(stderr, "  TCP:  nc localhost %d\n", UNIFIED_TCP_PORT);
    fprintf(stderr, "  UDP:  Use matching_engine_client --udp localhost %d\n", UNIFIED_UDP_PORT);
    fprintf(stderr, "\n");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char* argv[]) {
    /* Initialize config with defaults */
    unified_config_t config;
    unified_config_init(&config);
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--binary") == 0) {
            config.binary_default = true;
        }
        else if (strcmp(argv[i], "--quiet") == 0) {
            config.quiet_mode = true;
        }
        else if (strcmp(argv[i], "--single-processor") == 0) {
            config.single_processor = true;
        }
        else if (strcmp(argv[i], "--no-tcp") == 0) {
            config.disable_tcp = true;
        }
        else if (strcmp(argv[i], "--no-udp") == 0) {
            config.disable_udp = true;
        }
        else if (strcmp(argv[i], "--no-multicast") == 0) {
            config.disable_multicast = true;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    /* Validate configuration */
    if (config.disable_tcp && config.disable_udp) {
        fprintf(stderr, "Error: Cannot disable both TCP and UDP\n");
        return 1;
    }
    
    /* Setup signal handlers */
    setup_signal_handlers();
    
    /* Run the unified server */
    return run_unified_server(&config);
}

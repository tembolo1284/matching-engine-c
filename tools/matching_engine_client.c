/**
 * matching_engine_client.c - Main entry point for matching engine client
 *
 * A robust client for the matching engine that supports:
 *   - Auto-detection of transport (TCP/UDP) and encoding (Binary/CSV)
 *   - Interactive REPL mode
 *   - Predefined test scenarios
 *   - Optional multicast subscription for market data
 *   - Fire-and-forget mode for stress testing
 *
 * Usage:
 *   matching_engine_client [options] [host] [port]
 *
 * Examples:
 *   matching_engine_client localhost 1234
 *   matching_engine_client --scenario 2 localhost 1234
 *   matching_engine_client --multicast 239.255.0.1:5000 localhost 1234
 *   matching_engine_client --csv --scenario 11 localhost 1234
 */

#include "client/client_config.h"
#include "client/transport.h"
#include "client/codec.h"
#include "client/engine_client.h"
#include "client/scenarios.h"
#include "client/interactive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>

/* ============================================================
 * Version and Help
 * ============================================================ */

#define CLIENT_VERSION "1.0.0"

static void print_version(void) {
    printf("matching_engine_client version %s\n", CLIENT_VERSION);
}

static void print_usage(const char* program) {
    printf("\n");
    printf("Usage: %s [options] [host] [port]\n", program);
    printf("\n");
    printf("A robust client for the matching engine with auto-detection.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  host                Server hostname or IP (default: localhost)\n");
    printf("  port                Server port (default: 1234)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help          Show this help message\n");
    printf("  -V, --version       Show version\n");
    printf("  -v, --verbose       Verbose output\n");
    printf("  -q, --quiet         Suppress non-essential output\n");
    printf("\n");
    printf("Transport:\n");
    printf("  --tcp               Force TCP transport\n");
    printf("  --udp               Force UDP transport\n");
    printf("  (default: auto-detect, try TCP first)\n");
    printf("\n");
    printf("Encoding:\n");
    printf("  --binary            Force binary protocol\n");
    printf("  --csv               Force CSV protocol\n");
    printf("  (default: auto-detect via probe)\n");
    printf("\n");
    printf("Mode:\n");
    printf("  -s, --scenario ID   Run scenario instead of interactive mode\n");
    printf("  --fire-and-forget   Don't wait for responses (stress testing)\n");
    printf("  --danger-burst      Allow burst mode scenarios (40-41)\n");
    printf("  --list-scenarios    List available scenarios and exit\n");
    printf("\n");
    printf("Multicast:\n");
    printf("  -m, --multicast GROUP:PORT\n");
    printf("                      Subscribe to multicast market data feed\n");
    printf("                      Example: --multicast 239.255.0.1:5000\n");
    printf("  --multicast-only    Only subscribe to multicast (no order entry)\n");
    printf("\n");
    printf("Other:\n");
    printf("  -u, --user ID       User ID for orders (default: 1)\n");
    printf("  -t, --timeout MS    Connection timeout in milliseconds (default: 1000)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s localhost 1234\n", program);
    printf("  %s --scenario 2 localhost 1234\n", program);
    printf("  %s --tcp --binary localhost 1234\n", program);
    printf("  %s --multicast 239.255.0.1:5000 localhost 1234\n", program);
    printf("  %s --scenario 12 --fire-and-forget localhost 1234\n", program);
    printf("  %s --multicast-only --multicast 239.255.0.1:5000\n", program);
    printf("\n");
}

/* ============================================================
 * Argument Parsing
 * ============================================================ */

static struct option long_options[] = {
    /* Help */
    { "help",           no_argument,       NULL, 'h' },
    { "version",        no_argument,       NULL, 'V' },
    { "verbose",        no_argument,       NULL, 'v' },
    { "quiet",          no_argument,       NULL, 'q' },
    
    /* Transport */
    { "tcp",            no_argument,       NULL, 'T' },
    { "udp",            no_argument,       NULL, 'U' },
    
    /* Encoding */
    { "binary",         no_argument,       NULL, 'B' },
    { "csv",            no_argument,       NULL, 'C' },
    
    /* Mode */
    { "scenario",       required_argument, NULL, 's' },
    { "fire-and-forget",no_argument,       NULL, 'F' },
    { "danger-burst",   no_argument,       NULL, 'D' },
    { "list-scenarios", no_argument,       NULL, 'L' },
    
    /* Multicast */
    { "multicast",      required_argument, NULL, 'm' },
    { "multicast-only", no_argument,       NULL, 'M' },
    
    /* Other */
    { "user",           required_argument, NULL, 'u' },
    { "timeout",        required_argument, NULL, 't' },
    
    { NULL, 0, NULL, 0 }
};

static bool parse_multicast_arg(const char* arg, char* group, size_t group_size, uint16_t* port) {
    /* Format: GROUP:PORT (e.g., 239.255.0.1:5000) */
    const char* colon = strchr(arg, ':');
    if (colon == NULL) {
        return false;
    }
    
    size_t group_len = (size_t)(colon - arg);
    if (group_len >= group_size) {
        return false;
    }
    
    memcpy(group, arg, group_len);
    group[group_len] = '\0';
    
    *port = (uint16_t)atoi(colon + 1);
    return *port > 0;
}

static int parse_args(int argc, char* argv[], client_config_t* config, 
                      bool* list_scenarios) {
    int opt;
    
    *list_scenarios = false;
    
    while ((opt = getopt_long(argc, argv, "hVvqs:m:u:t:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return -1;
                
            case 'V':
                print_version();
                return -1;
                
            case 'v':
                config->verbose = true;
                break;
                
            case 'q':
                config->quiet = true;
                break;
                
            case 'T':  /* --tcp */
                config->transport = TRANSPORT_TCP;
                break;
                
            case 'U':  /* --udp */
                config->transport = TRANSPORT_UDP;
                break;
                
            case 'B':  /* --binary */
                config->encoding = ENCODING_BINARY;
                break;
                
            case 'C':  /* --csv */
                config->encoding = ENCODING_CSV;
                break;
                
            case 's':  /* --scenario */
                config->mode = MODE_SCENARIO;
                config->scenario_id = atoi(optarg);
                break;
                
            case 'F':  /* --fire-and-forget */
                config->fire_and_forget = true;
                break;
                
            case 'D':  /* --danger-burst */
                config->danger_burst = true;
                break;
                
            case 'L':  /* --list-scenarios */
                *list_scenarios = true;
                break;
                
            case 'm':  /* --multicast */
                config->multicast.enabled = true;
                if (!parse_multicast_arg(optarg, 
                                         config->multicast.group, 
                                         sizeof(config->multicast.group),
                                         &config->multicast.port)) {
                    fprintf(stderr, "Invalid multicast format: %s\n", optarg);
                    fprintf(stderr, "Expected: GROUP:PORT (e.g., 239.255.0.1:5000)\n");
                    return -1;
                }
                break;
                
            case 'M':  /* --multicast-only */
                config->mode = MODE_MULTICAST_ONLY;
                break;
                
            case 'u':  /* --user */
                config->user_id = (uint32_t)atoi(optarg);
                break;
                
            case 't':  /* --timeout */
                config->connect_timeout_ms = (uint32_t)atoi(optarg);
                config->recv_timeout_ms = config->connect_timeout_ms;
                break;
                
            default:
                print_usage(argv[0]);
                return -1;
        }
    }
    
    /* Parse positional arguments: [host] [port] */
    if (optind < argc) {
        strncpy(config->host, argv[optind], sizeof(config->host) - 1);
        config->host[sizeof(config->host) - 1] = '\0';
        optind++;
    }
    
    if (optind < argc) {
        config->port = (uint16_t)atoi(argv[optind]);
        optind++;
    }
    
    return 0;
}

/* ============================================================
 * Signal Handling
 * ============================================================ */

static volatile sig_atomic_t g_shutdown = 0;

static void shutdown_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

static void setup_signals(void) {
    struct sigaction sa;
    sa.sa_handler = shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ============================================================
 * Multicast-Only Mode
 * ============================================================ */

static void multicast_display_callback(const output_msg_t* msg, void* user_data) {
    (void)user_data;
    
    switch (msg->type) {
        case OUTPUT_MSG_ACK:
            printf("[ACK] %s user=%u order=%u\n",
                   msg->data.ack.symbol,
                   msg->data.ack.user_id,
                   msg->data.ack.user_order_id);
            break;
            
        case OUTPUT_MSG_CANCEL_ACK:
            printf("[CANCEL] %s user=%u order=%u\n",
                   msg->data.cancel_ack.symbol,
                   msg->data.cancel_ack.user_id,
                   msg->data.cancel_ack.user_order_id);
            break;
            
        case OUTPUT_MSG_TRADE:
            printf("[TRADE] %s buy=%u:%u sell=%u:%u price=%u qty=%u\n",
                   msg->data.trade.symbol,
                   msg->data.trade.user_id_buy,
                   msg->data.trade.user_order_id_buy,
                   msg->data.trade.user_id_sell,
                   msg->data.trade.user_order_id_sell,
                   msg->data.trade.price,
                   msg->data.trade.quantity);
            break;
            
        case OUTPUT_MSG_TOP_OF_BOOK:
            if (msg->data.top_of_book.price == 0 && 
                msg->data.top_of_book.total_quantity == 0) {
                printf("[TOB] %s %s EMPTY\n",
                       msg->data.top_of_book.symbol,
                       msg->data.top_of_book.side == SIDE_BUY ? "BID" : "ASK");
            } else {
                printf("[TOB] %s %s price=%u qty=%u\n",
                       msg->data.top_of_book.symbol,
                       msg->data.top_of_book.side == SIDE_BUY ? "BID" : "ASK",
                       msg->data.top_of_book.price,
                       msg->data.top_of_book.total_quantity);
            }
            break;
    }
}

static int run_multicast_only(engine_client_t* client) {
    printf("Multicast-only mode - listening for market data\n");
    printf("Press Ctrl+C to stop\n\n");
    
    engine_client_set_multicast_callback(client, multicast_display_callback, NULL);
    
    while (!g_shutdown) {
        engine_client_poll(client);
        
        /* Small sleep to avoid busy-wait */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };  /* 1ms */
        nanosleep(&ts, NULL);
    }
    
    printf("\n");
    engine_client_print_stats(client);
    
    return 0;
}

/* ============================================================
 * Main
 * ============================================================ */

int main(int argc, char* argv[]) {
    int exit_code = 0;
    
    /* Initialize configuration with defaults */
    client_config_t config;
    client_config_init(&config);
    
    /* Set default host */
    strcpy(config.host, "localhost");
    
    /* Parse command line */
    bool list_scenarios = false;
    if (parse_args(argc, argv, &config, &list_scenarios) != 0) {
        return (list_scenarios) ? 0 : 1;
    }
    
    /* Handle --list-scenarios */
    if (list_scenarios) {
        scenario_print_list();
        return 0;
    }
    
    /* Validate configuration */
    if (!client_config_validate(&config)) {
        if (config.mode == MODE_MULTICAST_ONLY && !config.multicast.enabled) {
            fprintf(stderr, "Error: --multicast-only requires --multicast GROUP:PORT\n");
        } else if (config.host[0] == '\0') {
            fprintf(stderr, "Error: No host specified\n");
        }
        return 1;
    }
    
    /* Set up signal handlers */
    setup_signals();
    
    /* Print banner (unless quiet) */
    if (!config.quiet) {
        printf("\n");
        printf("===========================================\n");
        printf("  Matching Engine Client v%s\n", CLIENT_VERSION);
        printf("===========================================\n");
        printf("\n");
        
        if (config.mode != MODE_MULTICAST_ONLY) {
            printf("Target:     %s:%u\n", config.host, config.port);
        }
        printf("Transport:  %s\n", transport_type_str(config.transport));
        printf("Encoding:   %s\n", encoding_type_str(config.encoding));
        printf("Mode:       %s\n", client_mode_str(config.mode));
        
        if (config.mode == MODE_SCENARIO) {
            const scenario_info_t* info = scenario_get_info(config.scenario_id);
            if (info) {
                printf("Scenario:   %d - %s\n", config.scenario_id, info->description);
            } else {
                printf("Scenario:   %d (unknown)\n", config.scenario_id);
            }
        }
        
        if (config.multicast.enabled) {
            printf("Multicast:  %s:%u\n", config.multicast.group, config.multicast.port);
        }
        
        if (config.fire_and_forget) {
            printf("Fire&Forget: enabled\n");
        }
        
        if (config.danger_burst) {
            printf("Burst Mode: ENABLED (danger!)\n");
        }
        
        printf("\n");
    }
    
    /* Initialize client */
    engine_client_t client;
    engine_client_init(&client, &config);
    
    /* Connect (or just set up multicast) */
    if (!engine_client_connect(&client)) {
        fprintf(stderr, "Failed to connect\n");
        return 1;
    }
    
    /* Run appropriate mode */
    switch (config.mode) {
        case MODE_INTERACTIVE: {
            interactive_options_t opts;
            interactive_options_init(&opts);
            opts.danger_burst = config.danger_burst;
            
            exit_code = interactive_run(&client, &opts);
            break;
        }
        
        case MODE_SCENARIO: {
            scenario_result_t result;
            if (!scenario_run(&client, config.scenario_id, config.danger_burst, &result)) {
                exit_code = 1;
            }
            break;
        }
        
        case MODE_MULTICAST_ONLY: {
            exit_code = run_multicast_only(&client);
            break;
        }
    }
    
    /* Cleanup */
    engine_client_disconnect(&client);
    engine_client_destroy(&client);
    
    return exit_code;
}

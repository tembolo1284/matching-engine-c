/**
 * interactive.c - Interactive REPL mode implementation
 */

#include "client/interactive.h"
#include "client/scenarios.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>

/* ============================================================
 * Constants
 * ============================================================ */

#define MAX_INPUT_LINE  1024
#define MAX_ARGS        16
#define MAX_ARG_LEN     128

/* ============================================================
 * Global State (for signal handling)
 * ============================================================ */

static volatile sig_atomic_t g_interrupted = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

/* ============================================================
 * Helper Functions
 * ============================================================ */

/**
 * Trim leading and trailing whitespace (modifies in place)
 */
static char* trim(char* str) {
    /* Leading whitespace */
    while (isspace((unsigned char)*str)) {
        str++;
    }
    
    if (*str == '\0') {
        return str;
    }
    
    /* Trailing whitespace */
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }
    end[1] = '\0';
    
    return str;
}

/**
 * Parse command line into arguments
 * Returns number of arguments parsed
 */
static int parse_args(char* line, char* args[], int max_args) {
    int argc = 0;
    char* token = strtok(line, " \t");
    
    while (token != NULL && argc < max_args) {
        args[argc++] = token;
        token = strtok(NULL, " \t");
    }
    
    return argc;
}

/**
 * Case-insensitive string comparison
 */
static int strcasecmp_local(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/**
 * Response callback for interactive mode
 */
static void interactive_response_callback(const output_msg_t* msg, void* user_data) {
    (void)user_data;
    
    switch (msg->type) {
        case OUTPUT_MSG_ACK:
            printf("  [ACK] %s user=%u order=%u\n",
                   msg->data.ack.symbol,
                   msg->data.ack.user_id,
                   msg->data.ack.user_order_id);
            break;
            
        case OUTPUT_MSG_CANCEL_ACK:
            printf("  [CANCEL] %s user=%u order=%u\n",
                   msg->data.cancel_ack.symbol,
                   msg->data.cancel_ack.user_id,
                   msg->data.cancel_ack.user_order_id);
            break;
            
        case OUTPUT_MSG_TRADE:
            printf("  [TRADE] %s buy=%u:%u sell=%u:%u price=%u qty=%u\n",
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
                printf("  [TOB] %s %s EMPTY\n",
                       msg->data.top_of_book.symbol,
                       msg->data.top_of_book.side == SIDE_BUY ? "BID" : "ASK");
            } else {
                printf("  [TOB] %s %s price=%u qty=%u\n",
                       msg->data.top_of_book.symbol,
                       msg->data.top_of_book.side == SIDE_BUY ? "BID" : "ASK",
                       msg->data.top_of_book.price,
                       msg->data.top_of_book.total_quantity);
            }
            break;
    }
}

/**
 * Multicast callback for interactive mode
 */
static void interactive_multicast_callback(const output_msg_t* msg, void* user_data) {
    (void)user_data;
    
    /* Prefix with [MCAST] to distinguish from direct responses */
    printf("  [MCAST] ");
    
    switch (msg->type) {
        case OUTPUT_MSG_ACK:
            printf("ACK %s user=%u order=%u\n",
                   msg->data.ack.symbol,
                   msg->data.ack.user_id,
                   msg->data.ack.user_order_id);
            break;
            
        case OUTPUT_MSG_CANCEL_ACK:
            printf("CANCEL %s user=%u order=%u\n",
                   msg->data.cancel_ack.symbol,
                   msg->data.cancel_ack.user_id,
                   msg->data.cancel_ack.user_order_id);
            break;
            
        case OUTPUT_MSG_TRADE:
            printf("TRADE %s %u@%u\n",
                   msg->data.trade.symbol,
                   msg->data.trade.quantity,
                   msg->data.trade.price);
            break;
            
        case OUTPUT_MSG_TOP_OF_BOOK:
            if (msg->data.top_of_book.price == 0) {
                printf("TOB %s %s EMPTY\n",
                       msg->data.top_of_book.symbol,
                       msg->data.top_of_book.side == SIDE_BUY ? "BID" : "ASK");
            } else {
                printf("TOB %s %s %u@%u\n",
                       msg->data.top_of_book.symbol,
                       msg->data.top_of_book.side == SIDE_BUY ? "BID" : "ASK",
                       msg->data.top_of_book.total_quantity,
                       msg->data.top_of_book.price);
            }
            break;
    }
}

/* ============================================================
 * Command Handlers
 * ============================================================ */

static bool cmd_buy(engine_client_t* client, int argc, char* args[],
                    const interactive_options_t* opts) {
    /* buy SYMBOL QTY@PRICE [order_id] */
    if (argc < 3) {
        printf("Usage: buy SYMBOL QTY@PRICE [order_id]\n");
        printf("Example: buy IBM 100@150\n");
        return true;
    }
    
    const char* symbol = args[1];
    
    /* Parse QTY@PRICE */
    uint32_t qty = 0, price = 0;
    if (sscanf(args[2], "%u@%u", &qty, &price) != 2) {
        printf("Invalid format. Use QTY@PRICE (e.g., 100@150)\n");
        return true;
    }
    
    uint32_t order_id = 0;
    if (argc > 3) {
        order_id = (uint32_t)atoi(args[3]);
    }
    
    uint32_t oid = engine_client_send_order(client, symbol, price, qty, SIDE_BUY, order_id);
    if (oid > 0) {
        printf("Sent BUY %s %u@%u (order_id=%u)\n", symbol, qty, price, oid);
        if (opts->auto_recv) {
            engine_client_recv_all(client, opts->recv_timeout_ms);
        }
    } else {
        printf("Failed to send order\n");
    }
    
    return true;
}

static bool cmd_sell(engine_client_t* client, int argc, char* args[],
                     const interactive_options_t* opts) {
    /* sell SYMBOL QTY@PRICE [order_id] */
    if (argc < 3) {
        printf("Usage: sell SYMBOL QTY@PRICE [order_id]\n");
        printf("Example: sell IBM 100@150\n");
        return true;
    }
    
    const char* symbol = args[1];
    
    /* Parse QTY@PRICE */
    uint32_t qty = 0, price = 0;
    if (sscanf(args[2], "%u@%u", &qty, &price) != 2) {
        printf("Invalid format. Use QTY@PRICE (e.g., 100@150)\n");
        return true;
    }
    
    uint32_t order_id = 0;
    if (argc > 3) {
        order_id = (uint32_t)atoi(args[3]);
    }
    
    uint32_t oid = engine_client_send_order(client, symbol, price, qty, SIDE_SELL, order_id);
    if (oid > 0) {
        printf("Sent SELL %s %u@%u (order_id=%u)\n", symbol, qty, price, oid);
        if (opts->auto_recv) {
            engine_client_recv_all(client, opts->recv_timeout_ms);
        }
    } else {
        printf("Failed to send order\n");
    }
    
    return true;
}

static bool cmd_cancel(engine_client_t* client, int argc, char* args[],
                       const interactive_options_t* opts) {
    /* cancel ORDER_ID */
    if (argc < 2) {
        printf("Usage: cancel ORDER_ID\n");
        return true;
    }
    
    uint32_t order_id = (uint32_t)atoi(args[1]);
    
    if (engine_client_send_cancel(client, order_id)) {
        printf("Sent CANCEL order_id=%u\n", order_id);
        if (opts->auto_recv) {
            engine_client_recv_all(client, opts->recv_timeout_ms);
        }
    } else {
        printf("Failed to send cancel\n");
    }
    
    return true;
}

static bool cmd_flush(engine_client_t* client, int argc, char* args[],
                      const interactive_options_t* opts) {
    (void)argc;
    (void)args;
    
    if (engine_client_send_flush(client)) {
        printf("Sent FLUSH\n");
        if (opts->auto_recv) {
            engine_client_recv_all(client, opts->recv_timeout_ms);
        }
    } else {
        printf("Failed to send flush\n");
    }
    
    return true;
}

static bool cmd_recv(engine_client_t* client, int argc, char* args[],
                     const interactive_options_t* opts) {
    (void)opts;
    
    int timeout_ms = 500;
    if (argc > 1) {
        timeout_ms = atoi(args[1]);
    }
    
    printf("Receiving (timeout=%d ms)...\n", timeout_ms);
    int count = engine_client_recv_all(client, timeout_ms);
    printf("Received %d messages\n", count);
    
    return true;
}

static bool cmd_poll(engine_client_t* client, int argc, char* args[],
                     const interactive_options_t* opts) {
    (void)argc;
    (void)args;
    (void)opts;
    
    int count = engine_client_poll(client);
    printf("Polled %d messages\n", count);
    
    return true;
}

static bool cmd_scenario(engine_client_t* client, int argc, char* args[],
                         const interactive_options_t* opts) {
    if (argc < 2) {
        printf("Usage: scenario ID\n\n");
        scenario_print_list();
        return true;
    }
    
    int scenario_id = atoi(args[1]);
    scenario_result_t result;
    
    if (!scenario_run(client, scenario_id, opts->danger_burst, &result)) {
        printf("Scenario failed or unknown\n");
    }
    
    return true;
}

static bool cmd_scenarios(engine_client_t* client, int argc, char* args[],
                          const interactive_options_t* opts) {
    (void)client;
    (void)argc;
    (void)args;
    (void)opts;
    
    scenario_print_list();
    return true;
}

static bool cmd_stats(engine_client_t* client, int argc, char* args[],
                      const interactive_options_t* opts) {
    (void)argc;
    (void)args;
    (void)opts;
    
    engine_client_print_stats(client);
    return true;
}

static bool cmd_reset(engine_client_t* client, int argc, char* args[],
                      const interactive_options_t* opts) {
    (void)argc;
    (void)args;
    (void)opts;
    
    engine_client_reset_stats(client);
    engine_client_reset_order_id(client, 1);
    printf("Statistics and order ID counter reset\n");
    return true;
}

static bool cmd_status(engine_client_t* client, int argc, char* args[],
                       const interactive_options_t* opts) {
    (void)argc;
    (void)args;
    (void)opts;
    
    printf("Connection Status:\n");
    printf("  Connected:    %s\n", engine_client_is_connected(client) ? "yes" : "no");
    printf("  Transport:    %s\n", transport_type_str(engine_client_get_transport(client)));
    printf("  Encoding:     %s\n", encoding_type_str(engine_client_get_encoding(client)));
    printf("  Next OrderID: %u\n", engine_client_peek_next_order_id(client));
    printf("  Multicast:    %s\n", client->multicast_active ? "active" : "inactive");
    
    return true;
}

static bool cmd_help(engine_client_t* client, int argc, char* args[],
                     const interactive_options_t* opts) {
    (void)client;
    (void)argc;
    (void)args;
    (void)opts;
    
    interactive_print_help();
    return true;
}

static bool cmd_examples(engine_client_t* client, int argc, char* args[],
                         const interactive_options_t* opts) {
    (void)client;
    (void)argc;
    (void)args;
    (void)opts;
    
    interactive_print_examples();
    return true;
}

static bool cmd_quit(engine_client_t* client, int argc, char* args[],
                     const interactive_options_t* opts) {
    (void)client;
    (void)argc;
    (void)args;
    (void)opts;
    
    printf("Goodbye!\n");
    return false;  /* Signal exit */
}

/* ============================================================
 * Command Dispatch
 * ============================================================ */

typedef bool (*cmd_handler_t)(engine_client_t*, int, char*[], const interactive_options_t*);

typedef struct {
    const char*     name;
    const char*     alias;
    cmd_handler_t   handler;
    const char*     description;
} command_t;

static const command_t COMMANDS[] = {
    { "buy",       "b",    cmd_buy,       "Send buy order: buy SYMBOL QTY@PRICE [order_id]" },
    { "sell",      "s",    cmd_sell,      "Send sell order: sell SYMBOL QTY@PRICE [order_id]" },
    { "cancel",    "c",    cmd_cancel,    "Cancel order: cancel ORDER_ID" },
    { "flush",     "f",    cmd_flush,     "Flush all orders" },
    { "recv",      "r",    cmd_recv,      "Receive responses: recv [timeout_ms]" },
    { "poll",      "p",    cmd_poll,      "Poll for messages (non-blocking)" },
    { "scenario",  "sc",   cmd_scenario,  "Run scenario: scenario ID" },
    { "scenarios", "list", cmd_scenarios, "List available scenarios" },
    { "stats",     NULL,   cmd_stats,     "Print statistics" },
    { "reset",     NULL,   cmd_reset,     "Reset statistics and order ID" },
    { "status",    NULL,   cmd_status,    "Show connection status" },
    { "help",      "h",    cmd_help,      "Show this help" },
    { "examples",  "ex",   cmd_examples,  "Show usage examples" },
    { "quit",      "q",    cmd_quit,      "Exit interactive mode" },
    { "exit",      NULL,   cmd_quit,      "Exit interactive mode" },
    { NULL, NULL, NULL, NULL }  /* Sentinel */
};

static const command_t* find_command(const char* name) {
    for (int i = 0; COMMANDS[i].name != NULL; i++) {
        if (strcasecmp_local(name, COMMANDS[i].name) == 0) {
            return &COMMANDS[i];
        }
        if (COMMANDS[i].alias && strcasecmp_local(name, COMMANDS[i].alias) == 0) {
            return &COMMANDS[i];
        }
    }
    return NULL;
}

/* ============================================================
 * Public API
 * ============================================================ */

void interactive_print_help(void) {
    printf("\n");
    printf("Matching Engine Client - Interactive Commands\n");
    printf("==============================================\n");
    printf("\n");
    
    printf("Order Entry:\n");
    for (int i = 0; COMMANDS[i].name != NULL; i++) {
        if (COMMANDS[i].handler == cmd_buy || 
            COMMANDS[i].handler == cmd_sell ||
            COMMANDS[i].handler == cmd_cancel ||
            COMMANDS[i].handler == cmd_flush) {
            if (COMMANDS[i].alias) {
                printf("  %-10s (%-2s)  %s\n", 
                       COMMANDS[i].name, COMMANDS[i].alias, COMMANDS[i].description);
            } else {
                printf("  %-10s       %s\n", 
                       COMMANDS[i].name, COMMANDS[i].description);
            }
        }
    }
    
    printf("\nReceiving:\n");
    for (int i = 0; COMMANDS[i].name != NULL; i++) {
        if (COMMANDS[i].handler == cmd_recv || 
            COMMANDS[i].handler == cmd_poll) {
            if (COMMANDS[i].alias) {
                printf("  %-10s (%-2s)  %s\n", 
                       COMMANDS[i].name, COMMANDS[i].alias, COMMANDS[i].description);
            } else {
                printf("  %-10s       %s\n", 
                       COMMANDS[i].name, COMMANDS[i].description);
            }
        }
    }
    
    printf("\nTesting:\n");
    for (int i = 0; COMMANDS[i].name != NULL; i++) {
        if (COMMANDS[i].handler == cmd_scenario || 
            COMMANDS[i].handler == cmd_scenarios) {
            if (COMMANDS[i].alias) {
                printf("  %-10s (%-2s)  %s\n", 
                       COMMANDS[i].name, COMMANDS[i].alias, COMMANDS[i].description);
            } else {
                printf("  %-10s       %s\n", 
                       COMMANDS[i].name, COMMANDS[i].description);
            }
        }
    }
    
    printf("\nInformation:\n");
    for (int i = 0; COMMANDS[i].name != NULL; i++) {
        if (COMMANDS[i].handler == cmd_stats || 
            COMMANDS[i].handler == cmd_reset ||
            COMMANDS[i].handler == cmd_status ||
            COMMANDS[i].handler == cmd_help ||
            COMMANDS[i].handler == cmd_examples) {
            if (COMMANDS[i].alias) {
                printf("  %-10s (%-2s)  %s\n", 
                       COMMANDS[i].name, COMMANDS[i].alias, COMMANDS[i].description);
            } else {
                printf("  %-10s       %s\n", 
                       COMMANDS[i].name, COMMANDS[i].description);
            }
        }
    }
    
    printf("\nControl:\n");
    for (int i = 0; COMMANDS[i].name != NULL; i++) {
        if (COMMANDS[i].handler == cmd_quit) {
            if (COMMANDS[i].alias) {
                printf("  %-10s (%-2s)  %s\n", 
                       COMMANDS[i].name, COMMANDS[i].alias, COMMANDS[i].description);
            } else {
                printf("  %-10s       %s\n", 
                       COMMANDS[i].name, COMMANDS[i].description);
            }
        }
    }
    
    printf("\n");
}

void interactive_print_examples(void) {
    printf("\n");
    printf("Examples:\n");
    printf("=========\n");
    printf("\n");
    printf("  # Place a buy order for 100 shares of IBM at $150\n");
    printf("  buy IBM 100@150\n");
    printf("\n");
    printf("  # Place a sell order (short form)\n");
    printf("  s AAPL 50@200\n");
    printf("\n");
    printf("  # Place order with specific order ID\n");
    printf("  buy NVDA 25@500 1001\n");
    printf("\n");
    printf("  # Cancel an order\n");
    printf("  cancel 1001\n");
    printf("  c 1001\n");
    printf("\n");
    printf("  # Flush all orders\n");
    printf("  flush\n");
    printf("  f\n");
    printf("\n");
    printf("  # Receive pending responses\n");
    printf("  recv\n");
    printf("  recv 1000    # 1 second timeout\n");
    printf("\n");
    printf("  # Run a test scenario\n");
    printf("  scenario 1   # Simple orders\n");
    printf("  scenario 2   # Matching trade\n");
    printf("  scenario 11  # 10K order stress test\n");
    printf("\n");
    printf("  # View statistics\n");
    printf("  stats\n");
    printf("\n");
}

bool interactive_execute(engine_client_t* client, 
                         const char* command,
                         const interactive_options_t* options) {
    /* Make mutable copy */
    char line[MAX_INPUT_LINE];
    strncpy(line, command, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    
    /* Trim whitespace */
    char* trimmed = trim(line);
    
    /* Skip empty lines and comments */
    if (trimmed[0] == '\0' || trimmed[0] == '#') {
        return true;
    }
    
    /* Parse arguments */
    char* args[MAX_ARGS];
    int argc = parse_args(trimmed, args, MAX_ARGS);
    
    if (argc == 0) {
        return true;
    }
    
    /* Find and execute command */
    const command_t* cmd = find_command(args[0]);
    
    if (cmd == NULL) {
        printf("Unknown command: %s (type 'help' for commands)\n", args[0]);
        return true;
    }
    
    return cmd->handler(client, argc, args, options);
}

int interactive_run(engine_client_t* client, const interactive_options_t* options) {
    char line[MAX_INPUT_LINE];
    
    /* Set up signal handler for Ctrl+C */
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    
    /* Set up callbacks */
    engine_client_set_response_callback(client, interactive_response_callback, NULL);
    if (client->multicast_active) {
        engine_client_set_multicast_callback(client, interactive_multicast_callback, NULL);
    }
    
    printf("\n");
    printf("Matching Engine Client - Interactive Mode\n");
    printf("Type 'help' for commands, 'quit' to exit\n");
    printf("\n");
    
    while (!g_interrupted) {
        /* Show prompt */
        if (options->show_prompt) {
            printf("> ");
            fflush(stdout);
        }
        
        /* Read line */
        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF */
            printf("\n");
            break;
        }
        
        /* Check for interrupt */
        if (g_interrupted) {
            printf("\nInterrupted\n");
            break;
        }
        
        /* Echo if requested */
        if (options->echo_commands) {
            printf(">> %s", line);
        }
        
        /* Execute */
        if (!interactive_execute(client, line, options)) {
            break;  /* quit command */
        }
        
        /* Poll for multicast data between commands */
        if (client->multicast_active) {
            engine_client_poll(client);
        }
    }
    
    /* Restore default signal handler */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGINT, &sa, NULL);
    
    return 0;
}

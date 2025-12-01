/**
 * interactive.h - Interactive REPL mode for matching engine client
 *
 * Provides a command-line interface for:
 *   - Sending orders, cancels, and flushes interactively
 *   - Viewing responses in real-time
 *   - Running scenarios on demand
 *   - Displaying statistics
 */

#ifndef CLIENT_INTERACTIVE_H
#define CLIENT_INTERACTIVE_H

#include "client/engine_client.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Interactive Mode Configuration
 * ============================================================ */

/**
 * Interactive mode options
 */
typedef struct {
    bool        show_prompt;        /* Show command prompt */
    bool        echo_commands;      /* Echo commands before execution */
    bool        auto_recv;          /* Automatically receive after send */
    int         recv_timeout_ms;    /* Timeout for auto-receive */
    bool        danger_burst;       /* Allow burst scenarios */
} interactive_options_t;

/**
 * Default interactive options
 */
static inline void interactive_options_init(interactive_options_t* opts) {
    opts->show_prompt = true;
    opts->echo_commands = false;
    opts->auto_recv = true;
    opts->recv_timeout_ms = 200;
    opts->danger_burst = false;
}

/* ============================================================
 * Interactive Mode API
 * ============================================================ */

/**
 * Run interactive REPL loop
 * 
 * Reads commands from stdin, executes them, and displays results.
 * Returns when user types 'quit' or 'exit', or on EOF.
 * 
 * @param client    Connected engine client
 * @param options   Interactive mode options
 * @return          0 on normal exit, -1 on error
 */
int interactive_run(engine_client_t* client, const interactive_options_t* options);

/**
 * Execute a single command string
 * 
 * @param client    Connected engine client
 * @param command   Command string to execute
 * @param options   Interactive mode options
 * @return          true to continue, false to exit
 */
bool interactive_execute(engine_client_t* client, 
                         const char* command,
                         const interactive_options_t* options);

/**
 * Print interactive mode help
 */
void interactive_print_help(void);

/**
 * Print command examples
 */
void interactive_print_examples(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_INTERACTIVE_H */

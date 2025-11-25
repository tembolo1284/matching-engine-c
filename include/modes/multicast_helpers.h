#ifndef MULTICAST_HELPERS_H
#define MULTICAST_HELPERS_H

#include <stdbool.h>
#include <pthread.h>
#include "network/multicast_publisher.h"
#include "threading/queues.h"
#include "modes/run_modes.h"

/**
 * Multicast helper context - encapsulates multicast publisher state
 */
typedef struct {
    multicast_publisher_context_t publisher_ctx;
    pthread_t thread;
    bool initialized;
    bool thread_started;
} multicast_helper_t;

/**
 * Setup multicast publisher (single processor mode)
 * 
 * @param helper Helper context to initialize
 * @param config Application configuration
 * @param output_queue Output queue to read from
 * @param shutdown_flag Shutdown coordination flag
 * @return true on success, false on error
 */
bool multicast_setup_single(multicast_helper_t* helper,
                             const app_config_t* config,
                             output_envelope_queue_t* output_queue,
                             atomic_bool* shutdown_flag);

/**
 * Setup multicast publisher (dual processor mode)
 * 
 * @param helper Helper context to initialize
 * @param config Application configuration
 * @param output_queue_0 Output queue from processor 0 (A-M)
 * @param output_queue_1 Output queue from processor 1 (N-Z)
 * @param shutdown_flag Shutdown coordination flag
 * @return true on success, false on error
 */
bool multicast_setup_dual(multicast_helper_t* helper,
                          const app_config_t* config,
                          output_envelope_queue_t* output_queue_0,
                          output_envelope_queue_t* output_queue_1,
                          atomic_bool* shutdown_flag);

/**
 * Start multicast publisher thread
 * 
 * @param helper Helper context
 * @return true on success, false on error
 */
bool multicast_start(multicast_helper_t* helper);

/**
 * Stop and cleanup multicast publisher
 * Waits for thread to finish and cleans up resources
 * 
 * @param helper Helper context
 */
void multicast_cleanup(multicast_helper_t* helper);

/**
 * Check if multicast is enabled in config
 * 
 * @param config Application configuration
 * @return true if multicast should be used
 */
static inline bool multicast_is_enabled(const app_config_t* config) {
    return config->enable_multicast;
}

#endif // MULTICAST_HELPERS_H

#include "modes/multicast_helpers.h"
#include <stdio.h>
#include <string.h>

bool multicast_setup_single(multicast_helper_t* helper,
                             const app_config_t* config,
                             output_envelope_queue_t* output_queue,
                             atomic_bool* shutdown_flag) {
    memset(helper, 0, sizeof(*helper));
    
    multicast_publisher_config_t mcast_config = {
        .port = config->multicast_port,
        .use_binary_output = config->binary_output,
        .ttl = 1  // 1 = local subnet, 32 = site-local, 255 = global
    };
    strncpy(mcast_config.multicast_group, config->multicast_group,
            sizeof(mcast_config.multicast_group) - 1);
    
    if (!multicast_publisher_init(&helper->publisher_ctx, &mcast_config,
                                  output_queue, shutdown_flag)) {
        fprintf(stderr, "[Multicast Helper] Failed to initialize publisher\n");
        return false;
    }
    
    helper->initialized = true;
    fprintf(stderr, "[Multicast Helper] Configured for %s:%u\n",
            config->multicast_group, config->multicast_port);
    
    return true;
}

bool multicast_setup_dual(multicast_helper_t* helper,
                          const app_config_t* config,
                          output_envelope_queue_t* output_queue_0,
                          output_envelope_queue_t* output_queue_1,
                          atomic_bool* shutdown_flag) {
    memset(helper, 0, sizeof(*helper));
    
    multicast_publisher_config_t mcast_config = {
        .port = config->multicast_port,
        .use_binary_output = config->binary_output,
        .ttl = 1
    };
    strncpy(mcast_config.multicast_group, config->multicast_group,
            sizeof(mcast_config.multicast_group) - 1);
    
    if (!multicast_publisher_init_dual(&helper->publisher_ctx, &mcast_config,
                                       output_queue_0, output_queue_1, shutdown_flag)) {
        fprintf(stderr, "[Multicast Helper] Failed to initialize dual-processor publisher\n");
        return false;
    }
    
    helper->initialized = true;
    fprintf(stderr, "[Multicast Helper] Configured for %s:%u (dual-processor)\n",
            config->multicast_group, config->multicast_port);
    
    return true;
}

bool multicast_start(multicast_helper_t* helper) {
    if (!helper->initialized) {
        fprintf(stderr, "[Multicast Helper] Cannot start - not initialized\n");
        return false;
    }
    
    if (pthread_create(&helper->thread, NULL, multicast_publisher_thread,
                       &helper->publisher_ctx) != 0) {
        fprintf(stderr, "[Multicast Helper] Failed to create thread\n");
        return false;
    }
    
    helper->thread_started = true;
    fprintf(stderr, "[Multicast Helper] Thread started successfully\n");
    
    return true;
}

void multicast_cleanup(multicast_helper_t* helper) {
    if (!helper->initialized) {
        return;
    }
    
    if (helper->thread_started) {
        fprintf(stderr, "[Multicast Helper] Waiting for thread to finish...\n");
        pthread_join(helper->thread, NULL);
        helper->thread_started = false;
    }
    
    multicast_publisher_cleanup(&helper->publisher_ctx);
    helper->initialized = false;
    
    fprintf(stderr, "[Multicast Helper] Cleanup complete\n");
}

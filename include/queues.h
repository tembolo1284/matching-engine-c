#ifndef MATCHING_ENGINE_QUEUES_H
#define MATCHING_ENGINE_QUEUES_H

#include "lockfree_queue.h"
#include "message_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Declare input message queue type */
DECLARE_LOCKFREE_QUEUE(input_msg_t, input_queue)

/* Declare output message queue type */
DECLARE_LOCKFREE_QUEUE(output_msg_t, output_queue)

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_QUEUES_H */

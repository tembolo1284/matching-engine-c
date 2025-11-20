#ifndef MATCHING_ENGINE_QUEUES_H
#define MATCHING_ENGINE_QUEUES_H

#include "threading/lockfree_queue.h"
#include "protocol/message_types_extended.h"
#include "protocol/message_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Declare input message queue type */
// DECLARE_LOCKFREE_QUEUE(input_msg_t, input_queue)

/* Declare output message queue type */
// DECLARE_LOCKFREE_QUEUE(output_msg_t, output_queue)

/**
 * Input envelope queue - unified queue for both UDP and TCP
 * Wraps messages with client_id and sequence number
 */
DECLARE_LOCKFREE_QUEUE(input_msg_envelope_t, input_envelope_queue)

/**
 * Output envelope queue - unified queue for both UDP and TCP
 * Wraps messages with client_id and sequence number
 */
DECLARE_LOCKFREE_QUEUE(output_msg_envelope_t, output_envelope_queue)

#ifdef __cplusplus
}
#endif

#endif /* MATCHING_ENGINE_QUEUES_H */

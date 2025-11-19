#include "queues.h"

/* Implement the lock-free queues */
IMPLEMENT_LOCKFREE_QUEUE(input_msg_t, input_queue)
IMPLEMENT_LOCKFREE_QUEUE(output_msg_t, output_queue)

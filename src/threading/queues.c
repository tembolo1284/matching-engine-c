#include "threading/queues.h"

// Define the envelope queue implementation
DEFINE_LOCKFREE_QUEUE(input_msg_envelope_t, input_envelope_queue)

DEFINE_LOCKFREE_QUEUE(output_msg_envelope_t, output_envelope_queue)

// ... other queue definitions to come maybe! Stay tuned ...

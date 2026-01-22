/*
 * pipeline.c - Pipeline depth control
 *
 * Uses a fixed high depth for maximum throughput.
 * Pipelining allows sending all batches before waiting for responses,
 * which is essential for high-latency connections.
 */

#include "pipeline.h"

/* Fixed depth - high enough to never block during send phase */
#define FIXED_DEPTH 1024

void pipeline_reset(void) {
    /* No state to reset */
}

void pipeline_adjust(double send_ms, double drain_ms, int batch_count) {
    /* No adaptive logic - just record for logging if needed */
    (void)send_ms;
    (void)drain_ms;
    (void)batch_count;
}

int pipeline_get_depth(void) {
    return FIXED_DEPTH;
}

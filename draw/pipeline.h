/*
 * pipeline.h - Adaptive pipeline depth control
 *
 * Manages the depth of pipelined 9P writes based on observed
 * send/drain time ratios. Goal: keep send time ≈ drain time.
 */

#ifndef PIPELINE_H
#define PIPELINE_H

/*
 * Pipeline depth limits.
 * MIN_DEPTH prevents starvation, MAX_DEPTH prevents memory bloat.
 */
#define PIPELINE_MIN_DEPTH 2
#define PIPELINE_MAX_DEPTH 200
#define PIPELINE_INITIAL_DEPTH 12

/*
 * Reset pipeline state.
 * Call when connection is reset or on startup.
 */
void pipeline_reset(void);

/*
 * Record timing from a completed frame and adjust depth.
 *
 * send_ms:     Time spent sending all batches (before waiting for responses)
 * drain_ms:    Time spent collecting pipelined responses
 * batch_count: Number of batches sent this frame
 *
 * The algorithm uses weighted rolling averages:
 * - Recent frames weighted more than old
 * - Heavy frames (more batches) weighted more than light
 *
 * Adjustment logic:
 * - ratio < 0.5: send << drain, depth too high, decrease
 * - ratio > 2.0: send >> drain, depth too low, increase
 * - 0.5 <= ratio <= 2.0: balanced, no change
 */
void pipeline_adjust(double send_ms, double drain_ms, int batch_count);

/*
 * Get current pipeline depth.
 * Use this to decide how many writes to buffer before draining.
 */
int pipeline_get_depth(void);

#endif /* PIPELINE_H */

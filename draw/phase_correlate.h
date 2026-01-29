/*
 * phase_correlate.h - FFT-based motion detection using phase correlation
 */

#ifndef PHASE_CORRELATE_H
#define PHASE_CORRELATE_H

#include <stdint.h>

/* FFT window size (full resolution) - actual FFT size = FFT_SIZE / k */
#define FFT_SIZE 256

/* Result of phase correlation */
struct phase_result {
    int dx;      /* Detected horizontal shift (k-aligned) */
    int dy;      /* Detected vertical shift (k-aligned) */
    int valid;   /* Whether detection succeeded */
};

/*
 * Initialize thread-local FFT resources.
 * Called automatically on first use, but can be called explicitly.
 * Returns 0 on success, -1 on failure.
 */
int phase_correlate_init(void);

/*
 * Detect scroll offset between current and previous frame regions.
 *
 * Parameters:
 *   curr_buf, prev_buf - Frame buffers (XRGB32 format)
 *   buf_width - Width of frame buffers in pixels
 *   rx1, ry1, rx2, ry2 - Region bounds to analyze
 *   max_shift - Maximum scroll offset to detect
 *   k - Integer output scale = ceil(scale). Scroll offsets are k-aligned.
 *       Uses k-based downsampling: FFT operates at (FFT_SIZE/k) resolution.
 *       Valid range: 1-4.
 *
 * Returns:
 *   phase_result with detected dx, dy (multiples of k) and validity flag.
 *   dx/dy are 0 if no scroll detected.
 */
struct phase_result phase_correlate_detect(
    uint32_t *curr_buf, uint32_t *prev_buf, int buf_width,
    int rx1, int ry1, int rx2, int ry2,
    int max_shift, int k
);

/*
 * Cleanup all FFT resources across all threads.
 * Call at program exit.
 */
void phase_correlate_cleanup(void);

#endif /* PHASE_CORRELATE_H */

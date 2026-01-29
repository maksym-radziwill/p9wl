/*
 * phase_correlate.h - FFT-based scroll detection
 */

#ifndef PHASE_CORRELATE_H
#define PHASE_CORRELATE_H

#include <stdint.h>

/* FFT size - must be power of 2, larger = more accurate but slower */
#define FFT_SIZE 256

/* Maximum scroll distance to detect (pixels) */
#define MAX_SCROLL_PIXELS (MAX_SCROLL_REGIONS / 2)

/* Result of phase correlation */
struct phase_result {
    int dx;           /* Detected horizontal shift */
    int dy;           /* Detected vertical shift */
    int valid;        /* Non-zero if detection succeeded */
};

/* Initialize thread-local FFT resources (called automatically) */
int phase_correlate_init(void);

/* Detect scroll between current and previous frame regions
 * Returns shift (dx, dy) with confidence measure
 */
struct phase_result phase_correlate_detect(
    uint32_t *curr_buf, uint32_t *prev_buf, int buf_width,
    int rx1, int ry1, int rx2, int ry2,
    int max_shift
);

/* Free all FFT resources (call at shutdown) */
void phase_correlate_cleanup(void);

#endif /* PHASE_CORRELATE_H */

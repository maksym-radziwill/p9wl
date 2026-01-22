/*
 * scroll.h - Scroll detection and 9P scroll commands
 *
 * Detects scrolling regions in frame buffers and sends
 * efficient blit commands to the Plan 9 draw device.
 */

#ifndef SCROLL_H
#define SCROLL_H

#include <stdint.h>

/* Forward declarations */
struct server;

/* Minimum and maximum scroll amounts to detect */
#define MIN_SCROLL_PIXELS 1

/* Scroll region state */
struct scroll_region {
    int x1, y1, x2, y2;   /* Region bounds */
    int detected;          /* Non-zero if scroll detected */
    int dx, dy;           /* Detected scroll offset */
};

/*
 * Detect scrolling in all regions of the frame.
 *
 * Divides the frame into regions and uses FFT-based phase correlation
 * to detect translation between current and previous frame.
 * Results are stored in s->scroll_regions[].
 *
 * Uses thread pool for parallel processing.
 */
void detect_scroll(struct server *s, uint32_t *send_buf);

/*
 * Send 9P blit commands for detected scroll regions.
 *
 * For each region where scroll was detected, sends a 'd' (draw) command
 * to blit the scrolled content, avoiding the need to retransmit unchanged pixels.
 *
 * pending_writes: incremented for each pipelined write sent
 *
 * Returns number of scroll regions processed.
 */
int send_scroll_commands(struct server *s, int *pending_writes);

/*
 * Timing statistics from the last detect_scroll() call.
 */
struct scroll_timing {
    double total_us;
    double extract_us;
    double fft_us;
    double correlation_us;
    double peak_us;
    double verify_us;
    int regions_processed;
    int regions_detected;
};

/*
 * Get timing statistics from the last detect_scroll() call.
 * Returns pointer to internal struct (valid until next detect_scroll call).
 */
const struct scroll_timing *scroll_get_timing(void);

/*
 * Initialize scroll detection resources.
 * Called automatically on first use.
 */
void scroll_init(void);

/*
 * Cleanup scroll detection resources.
 * Call at program exit for clean shutdown.
 */
void scroll_cleanup(void);

#endif /* SCROLL_H */

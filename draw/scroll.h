/*
 * scroll.h - Scroll detection and 9P scroll commands
 *
 * Detects scrolling regions in frame buffers and sends
 * efficient blit commands to the Plan 9 draw device.
 *
 * Uses FFT-based phase correlation to detect translation between
 * frames. When scrolling is detected, generates 'd' (draw/copy)
 * commands instead of retransmitting pixel data, significantly
 * reducing bandwidth for scrolling content.
 *
 * IMPORTANT: Include types.h before this header.
 * Scroll region data is stored in server->scroll_regions[].
 */

#ifndef SCROLL_H
#define SCROLL_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
struct server;

/* Minimum scroll amount to detect (pixels) */
#define MIN_SCROLL_PIXELS 1

/*
 * Detect scrolling in all regions of the frame.
 *
 * Divides the frame into a grid of regions (stored in s->scroll_regions)
 * and uses FFT-based phase correlation to detect translation between
 * the current frame (send_buf) and previous frame (s->prev_framebuf).
 *
 * For each region where scrolling is detected:
 *   - s->scroll_regions[i].detected is set to 1
 *   - s->scroll_regions[i].dx, dy contain the scroll offset
 *
 * Uses thread pool for parallel processing of regions.
 *
 * s:        server state with prev_framebuf populated
 * send_buf: current frame pixel buffer
 */
void detect_scroll(struct server *s, uint32_t *send_buf);

/*
 * Apply detected scroll to prev_framebuf.
 *
 * For each region with detected scrolling, shifts pixels in
 * s->prev_framebuf and marks exposed areas with 0xDEADBEEF.
 *
 * Must be called BEFORE tile change detection so tiles compare
 * against the post-scroll state. This allows delta compression
 * to work efficiently on scrolled content.
 *
 * s: server state with scroll_regions populated by detect_scroll()
 *
 * Returns number of scroll regions applied.
 */
int apply_scroll_to_prevbuf(struct server *s);

/*
 * Write scroll 'd' commands to batch buffer.
 *
 * Generates Plan 9 draw 'd' (copy) commands for each detected
 * scroll region. These commands tell the draw server to copy
 * pixels within the framebuffer image, avoiding retransmission.
 *
 * Does NOT update prev_framebuf - call apply_scroll_to_prevbuf first.
 *
 * s:        server state with scroll_regions populated
 * batch:    buffer to write commands to
 * max_size: maximum bytes to write
 *
 * Returns number of bytes written to batch.
 */
int write_scroll_commands(struct server *s, uint8_t *batch, size_t max_size);

/*
 * Timing statistics from the last detect_scroll() call.
 *
 * Used for performance monitoring and debugging.
 */
struct scroll_timing {
    double total_us;         /* Total time for detect_scroll() */
    double extract_us;       /* Time extracting regions */
    double fft_us;           /* Time in FFT operations */
    double correlation_us;   /* Time computing correlation */
    double peak_us;          /* Time finding correlation peak */
    double verify_us;        /* Time verifying scroll benefit */
    int regions_processed;   /* Number of regions analyzed */
    int regions_detected;    /* Number of regions with detected scroll */
};

/*
 * Get timing statistics from the last detect_scroll() call.
 *
 * Returns pointer to internal struct, valid until next detect_scroll().
 * Do not free the returned pointer.
 */
const struct scroll_timing *scroll_get_timing(void);

/*
 * Initialize scroll detection resources.
 *
 * Currently a no-op - resources are initialized lazily.
 * Provided for symmetry with scroll_cleanup().
 */
void scroll_init(void);

/*
 * Cleanup scroll detection resources.
 *
 * Shuts down the parallel thread pool and releases FFT resources.
 * Call at program exit for clean shutdown.
 */
void scroll_cleanup(void);

#endif /* SCROLL_H */

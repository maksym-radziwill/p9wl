/*
 * scroll.h - Scroll detection and 9P scroll commands
 *
 * Detects scrolling regions in frame buffers and generates efficient
 * blit commands for the Plan 9 draw device. Uses FFT-based phase
 * correlation to detect translation between frames.
 *
 * Architecture Overview:
 *
 *   Scroll detection runs in the send thread before tile compression:
 *
 *     detect_scroll()
 *           │
 *           ├─► Divide frame into grid of regions
 *           │
 *           ├─► parallel_for() over regions
 *           │     └─► phase_correlate_detect() per region
 *           │     └─► Verify scroll benefit via compression test
 *           │
 *           └─► Store results in s->scroll_regions[]
 *
 *     apply_scroll_to_prevbuf()
 *           │
 *           └─► Shift pixels in prev_framebuf to match scroll
 *               Mark exposed areas with 0xDEADBEEF
 *
 *     write_scroll_commands()
 *           │
 *           └─► Generate 'd' (copy) commands for detected scrolls
 *
 * Region Grid:
 *
 *   The frame is divided into a grid of analysis regions:
 *
 *     ┌─────────┬─────────┐
 *     │ Region  │ Region  │
 *     │  (0,0)  │  (1,0)  │
 *     ├─────────┼─────────┤
 *     │ Region  │ Region  │
 *     │  (0,1)  │  (1,1)  │
 *     └─────────┴─────────┘
 *
 *   Grid parameters:
 *     - cols = width / 256 (at least 1)
 *     - rows = height / 256 (at least 1)
 *     - margin = TILE_SIZE around edges (naturally excludes padding)
 *     - Regions are tile-aligned (multiples of TILE_SIZE)
 *     - width/height are the padded buffer dimensions (TILE_ALIGN_UP)
 *
 *   Each region is analyzed independently, allowing different scroll
 *   vectors in different parts of the screen (e.g., two scrolling
 *   panes in a split-screen layout).
 *
 * Phase Correlation:
 *
 *   For each region, phase_correlate_detect() computes the translation
 *   between the same region in current and previous frames:
 *
 *     1. Extract region from both frames
 *     2. Apply Hann window to reduce edge effects
 *     3. Compute 2D FFT of both regions
 *     4. Cross-correlate in frequency domain
 *     5. Inverse FFT to find correlation peak
 *     6. Peak offset = (dx, dy) scroll vector
 *
 *   See phase_correlate.h for algorithm details.
 *
 * Scroll Verification:
 *
 *   After detecting a scroll vector, the algorithm verifies it would
 *   actually reduce bandwidth before accepting it:
 *
 *     For each tile in the region:
 *       - Compute compression cost WITHOUT scroll (vs prev_framebuf)
 *       - Compute compression cost WITH scroll (vs shifted prev_framebuf)
 *
 *     If bytes_with_scroll > bytes_no_scroll:
 *       - Reject the scroll (false positive or not beneficial)
 *
 *   This verification catches cases where:
 *     - Phase correlation found noise, not real scroll
 *     - Scroll exists but content changed significantly
 *     - Compression artifacts make scroll not worthwhile
 *
 * Scroll Rectangles:
 *
 *   compute_scroll_rects() (in draw_helpers.h) computes:
 *
 *     For scrolling down by dy pixels:
 *
 *       ┌────────────────┐
 *       │   src region   │ ─┐
 *       │                │  │ Copy
 *       ├────────────────┤  │ down
 *       │   dst region   │ ◄┘
 *       ├────────────────┤
 *       │ exposed region │ ◄── Needs fresh content
 *       └────────────────┘
 *
 *     src: Where to copy from in the source buffer
 *     dst: Where to copy to in the destination
 *     exp: Newly exposed area (tile-aligned)
 *
 * prev_framebuf Update:
 *
 *   apply_scroll_to_prevbuf() modifies prev_framebuf to reflect the
 *   scroll that will be applied server-side:
 *
 *     1. Shift pixels within prev_framebuf (using memmove)
 *     2. Mark exposed regions with 0xDEADBEEF marker
 *
 *   This allows tile change detection to compare against the post-scroll
 *   state, enabling efficient delta encoding for the non-scrolled tiles.
 *
 *   The 0xDEADBEEF marker tells the tile encoder to skip delta encoding
 *   for those tiles (no valid reference exists).
 *
 * Draw Commands:
 *
 *   write_scroll_commands() generates Plan 9 'd' (draw/copy) commands:
 *
 *     'd' image_id image_id opaque_id dst_rect src_point
 *
 *   This tells the draw server to copy pixels within the framebuffer
 *   image, avoiding retransmission of scrolled content.
 *
 * Scroll Disabling:
 *
 *   Scroll detection is disabled when:
 *     - s->scale has a fractional component (non-integer scaling)
 *     - s->prev_framebuf is NULL (first frame)
 *     - force_full_frame is set
 *
 *   Non-integer scaling breaks pixel-accurate phase correlation.
 *
 * Timing Statistics:
 *
 *   scroll_get_timing() returns performance metrics for debugging:
 *     - total_us: Total time for detect_scroll()
 *     - regions_processed: Number of regions analyzed
 *     - regions_detected: Number with detected scroll
 *
 * Thread Safety:
 *
 *   - detect_scroll() uses parallel_for() internally
 *   - Each region is processed by one worker thread
 *   - Results are written to per-region slots (no contention)
 *   - The timing struct is updated only by the send thread
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

/* ============== Constants ============== */

/* Minimum scroll amount to detect (pixels) */
#define MIN_SCROLL_PIXELS 1

/* ============== Scroll Detection ============== */

/*
 * Detect scrolling in all regions of the frame.
 *
 * Divides the frame into a grid of regions and uses FFT-based phase
 * correlation to detect translation between the current frame and
 * the previous frame.
 *
 * Processing steps:
 *   1. Compute region grid based on frame dimensions
 *   2. For each region (in parallel):
 *      a. Run phase correlation to detect (dx, dy)
 *      b. Skip if dx=0 and dy=0
 *      c. Verify scroll benefit via compression cost comparison
 *      d. Store result in s->scroll_regions[i]
 *
 * Results are stored in:
 *   - s->scroll_regions[i].detected: 1 if scroll found, 0 otherwise
 *   - s->scroll_regions[i].dx, dy: scroll offset (if detected)
 *   - s->num_scroll_regions: total regions analyzed
 *   - s->scroll_regions_x, _y: grid dimensions
 *
 * s:        server state with prev_framebuf populated
 * send_buf: current frame pixel buffer (XRGB32)
 *
 * Preconditions:
 *   - s->prev_framebuf must be valid and same size as send_buf
 *   - s->width, s->height must be set
 */
void detect_scroll(struct server *s, uint32_t *send_buf);

/*
 * Apply detected scroll to prev_framebuf.
 *
 * For each region with detected scrolling, modifies s->prev_framebuf
 * to match the state that will exist after scroll commands execute
 * on the server:
 *
 *   1. Shift pixels within prev_framebuf (memmove for overlap safety)
 *   2. Mark exposed areas with 0xDEADBEEF sentinel value
 *
 * The 0xDEADBEEF marker indicates pixels that have no valid reference
 * for delta encoding. The tile encoder checks for this marker and
 * disables delta encoding for affected tiles.
 *
 * Must be called AFTER detect_scroll() and BEFORE tile change detection.
 * This ensures tiles are compared against the post-scroll state.
 *
 * s: server state with scroll_regions populated by detect_scroll()
 *
 * Returns number of scroll regions applied (regions with detected=1).
 */
int apply_scroll_to_prevbuf(struct server *s);

/*
 * Write scroll 'd' commands to batch buffer.
 *
 * Generates Plan 9 draw 'd' (copy) commands for each detected
 * scroll region. These commands instruct the draw server to copy
 * pixels within the framebuffer image.
 *
 * Command format:
 *   'd' dst_id src_id mask_id dst_rect src_point mask_point
 *
 * For scroll: dst_id = src_id = image_id (copy within same image)
 *
 * Does NOT update prev_framebuf - call apply_scroll_to_prevbuf() first.
 * Does NOT include flush command - caller must append if needed.
 *
 * s:        server state with scroll_regions populated
 * batch:    buffer to write commands to
 * max_size: maximum bytes to write
 *
 * Returns number of bytes written to batch.
 * Returns 0 if no scroll regions detected.
 * Logs error and stops early if batch would overflow.
 */
int write_scroll_commands(struct server *s, uint8_t *batch, size_t max_size);

/* ============== Timing Statistics ============== */

/*
 * Timing statistics from the last detect_scroll() call.
 *
 * Updated by detect_scroll() at the start and end of each call.
 * Fields are zeroed at the beginning of each detect_scroll() invocation.
 */
struct scroll_timing {
    double total_us;         /* Total time for detect_scroll() */
    int regions_processed;   /* Number of regions analyzed */
    int regions_detected;    /* Number of regions with detected scroll */
};

/*
 * Get timing statistics from the last detect_scroll() call.
 *
 * Returns pointer to internal static struct. Valid until the next
 * detect_scroll() call. Do not free the returned pointer.
 *
 * Before detect_scroll() has been called, returns a pointer to a
 * zeroed struct (all fields are 0).
 */
const struct scroll_timing *scroll_get_timing(void);

/* ============== Lifecycle ============== */

/*
 * Initialize scroll detection resources.
 *
 * Currently a no-op - resources are initialized lazily by
 * detect_scroll() and phase_correlate_detect().
 *
 * Provided for API symmetry with scroll_cleanup().
 */
void scroll_init(void);

/*
 * Cleanup scroll detection resources.
 *
 * Shuts down resources used by scroll detection:
 *   - Calls parallel_cleanup() to stop worker threads
 *   - Calls phase_correlate_cleanup() to free FFT resources
 *
 * Safe to call even if scroll_init() was never called.
 * Call at program exit for clean shutdown.
 */
void scroll_cleanup(void);

#endif /* SCROLL_H */

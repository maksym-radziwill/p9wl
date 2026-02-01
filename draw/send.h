/*
 * send.h - Frame sending and send thread
 *
 * Handles queuing frames from the compositor thread and sending them
 * over 9P to the Plan 9 draw device. Uses pipelined writes for high
 * throughput and parallel tile compression for efficiency.
 *
 * Architecture Overview:
 *
 *   The send system uses a producer-consumer model with three threads:
 *
 *     Compositor Thread (producer):
 *       - Renders frames to s->framebuf
 *       - Calls send_frame() to queue for transmission
 *       - Never blocks waiting for network I/O
 *
 *     Send Thread (consumer):
 *       - Waits for frames via s->send_cond
 *       - Detects changed tiles and scrolling
 *       - Compresses tiles in parallel
 *       - Batches commands and sends via 9P
 *       - Updates s->prev_framebuf for delta encoding
 *
 *     Drain Thread (I/O helper):
 *       - Reads 9P Rwrite responses asynchronously
 *       - Allows pipelined writes without blocking
 *       - Handles error detection and recovery
 *
 * Double Buffering:
 *
 *   Uses two send buffers (s->send_buf[0], s->send_buf[1]) to decouple
 *   the compositor from network transmission:
 *
 *     s->pending_buf: Buffer just queued, waiting for send thread
 *     s->active_buf:  Buffer currently being transmitted
 *
 *   send_frame() finds a free buffer (neither pending nor active),
 *   copies the framebuffer, and signals the send thread.
 *
 * Pipelined I/O:
 *
 *   To maximize throughput, writes are pipelined:
 *
 *     1. Send thread calls p9_write_send() (non-blocking)
 *     2. drain_notify() signals drain thread
 *     3. Drain thread reads Rwrite response asynchronously
 *     4. Send thread continues with next batch
 *
 *   The drain_throttle() function prevents unbounded pipelining
 *   by waiting when too many responses are pending.
 *
 * Frame Processing Pipeline:
 *
 *   For each frame, the send thread:
 *
 *     1. Error Recovery:
 *        - Check for draw_error (protocol errors)
 *        - Check for unknown_id_error (window moved)
 *        - Check for drain errors (I/O failures)
 *        - On error: invalidate prev_framebuf, force full frame
 *
 *     2. Window Updates:
 *        - If window_changed, pause drain and relookup window
 *        - If resize_pending after relookup, skip frame
 *
 *     3. Scroll Detection (if enabled):
 *        - Call detect_scroll() to find scrolling regions
 *        - Call apply_scroll_to_prevbuf() to update reference
 *        - Generate scroll 'd' commands
 *
 *     4. Tile Change Detection:
 *        - Compare send_buf against prev_framebuf
 *        - Build list of changed tiles (struct tile_work)
 *        - Skip tiles in scroll-exposed regions for delta encoding
 *
 *     5. Parallel Compression:
 *        - Call compress_tiles_parallel() on changed tiles
 *        - Each tile tries both direct and alpha-delta encoding
 *        - Result indicates which encoding was smaller
 *
 *     6. Batch Building:
 *        - Collect compressed tiles into batch buffer
 *        - Flush batch when full (max_batch bytes)
 *        - Use 'Y' command for compressed, 'y' for raw
 *        - For delta tiles: load to delta_id, composite to image_id
 *
 *     7. Final Batch:
 *        - 'd' command to copy image_id to screen_id
 *        - Border fill commands (if window has margins)
 *        - 'v' flush command to display
 *
 *     8. State Update:
 *        - Copy sent tiles to prev_framebuf
 *        - Enable alpha-delta mode after first successful frame
 *
 * Alpha-Delta Mode:
 *
 *   After the first successful frame, draw->xor_enabled is set to 1,
 *   enabling alpha-delta encoding for subsequent frames. This mode:
 *
 *     - Creates sparse ARGB32 delta buffers (see compress.h)
 *     - Compresses deltas to delta_id image
 *     - Uses 'd' command to composite onto image_id
 *     - Significantly reduces bandwidth for small changes
 *
 *   The 0xDEADBEEF marker in prev_framebuf indicates scroll-exposed
 *   regions where delta encoding should be disabled (no valid reference).
 *
 * Border Drawing:
 *
 *   When the window is larger than the tile-aligned content area,
 *   write_borders() fills the margins with border_id color:
 *
 *     +---------------------------+
 *     |      top border           |
 *     +---+---------------+-------+
 *     | L |               |   R   |
 *     | E |   content     |   I   |
 *     | F |     area      |   G   |
 *     | T |               |   H   |
 *     |   |               |   T   |
 *     +---+---------------+-------+
 *     |     bottom border         |
 *     +---------------------------+
 *
 * Scroll Disabling:
 *
 *   Scroll detection is disabled when s->scale has a fractional
 *   component (non-integer scaling). This is because phase correlation
 *   requires pixel-accurate alignment that fractional scaling breaks.
 *
 * Thread Safety:
 *
 *   s->send_lock protects:
 *     - s->pending_buf, s->active_buf
 *     - s->send_full, s->force_full_frame
 *     - s->resize_pending
 *
 *   The drain thread uses atomic operations for its counters.
 */

#ifndef SEND_H
#define SEND_H

#include <stdint.h>

/* Forward declarations */
struct server;

/* Tile size for change detection and compression */
#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

/* ============== Frame Queueing ============== */

/*
 * Queue a frame for sending.
 *
 * Copies the current framebuffer (s->framebuf) to a send buffer and
 * signals the send thread. Uses double-buffering to avoid blocking
 * the compositor while the previous frame is being transmitted.
 *
 * Buffer selection:
 *   - Finds a buffer that is neither pending nor active
 *   - If no buffer is free, the frame is dropped (throttling)
 *
 * Flags copied:
 *   - If s->force_full_frame is set, s->send_full is set
 *
 * This function returns immediately after copying; actual transmission
 * happens asynchronously in the send thread.
 *
 * Thread-safe: acquires s->send_lock internally.
 *
 * s: server state containing framebuffer and send thread context
 *
 * Preconditions:
 *   - s->framebuf must be valid
 *   - s->width * s->height * 4 bytes will be copied
 */
void send_frame(struct server *s);

/* ============== Timer Integration ============== */

/*
 * Timer callback for throttled frame sending.
 *
 * Called by the Wayland event loop at the configured frame rate
 * (typically 30-60 Hz). Checks if there are pending changes and
 * triggers send_frame() if so.
 *
 * This provides frame rate limiting - even if the compositor renders
 * faster, frames are only sent at the timer rate.
 *
 * data: pointer to struct server
 *
 * Returns 0 (Wayland event loop convention for success).
 */
int send_timer_callback(void *data);

/* ============== Send Thread ============== */

/*
 * Send thread main function.
 *
 * Runs in a dedicated thread, processing queued frames. Starts the
 * drain thread for asynchronous I/O and allocates compression buffers.
 *
 * Main loop behavior:
 *
 *   1. Wait for frame via pthread_cond_wait on s->send_cond
 *   2. Handle errors (draw_error, unknown_id_error, drain errors)
 *   3. Handle window changes (relookup, resize detection)
 *   4. Detect and apply scroll transformations
 *   5. Identify changed tiles by comparing with prev_framebuf
 *   6. Compress tiles in parallel (direct and alpha-delta)
 *   7. Build batches of draw commands
 *   8. Send batches via 9P with pipelined writes
 *   9. Update prev_framebuf for next frame
 *
 * Exit conditions:
 *   - s->running becomes false
 *   - Fatal 9P connection error
 *
 * Cleanup on exit:
 *   - Stops drain thread
 *   - Shuts down compression pool
 *   - Frees work arrays and buffers
 *
 * arg: pointer to struct server
 *
 * Returns NULL on thread exit.
 */
void *send_thread_func(void *arg);

#endif /* SEND_H */

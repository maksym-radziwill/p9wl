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
 *       - Signals done_cond after each completed response to wake
 *         drain_throttle() and drain_pause() without polling
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
 *   swaps the framebuffer pointer with the free send buffer (zero-copy),
 *   copies the dirty tile bitmap from staging, and signals the send
 *   thread. All three buffers (framebuf, send_buf[0], send_buf[1])
 *   must be allocated at the padded size (TILE_ALIGN_UP of visible
 *   dimensions) since their pointers are swapped.
 *
 * Damage-Based Dirty Tile Tracking:
 *
 *   To avoid scanning every tile for changes, the output thread
 *   extracts compositor damage into a per-tile bitmap:
 *
 *     s->dirty_staging:       Written by output thread (no lock needed)
 *     s->dirty_staging_valid: Set when staging has valid damage data
 *     s->dirty_tiles[N]:      Per-send-buffer bitmap (copied under lock)
 *     s->dirty_valid[N]:      Whether bitmap is valid for buffer N
 *
 *   The bitmap size is tiles_x × tiles_y where tiles_x = width/TILE_SIZE
 *   and tiles_y = height/TILE_SIZE, using the padded (TILE_ALIGN_UP)
 *   dimensions.  Tiles in the padding region are never marked dirty
 *   by the compositor (no visible content touches them), so they are
 *   effectively free.
 *
 *   Flow:
 *     1. output_frame() checks scene_dirty flag — if scene hasn't
 *        changed, skips rendering entirely (no build_state, no copy)
 *     2. When rendering occurs, extracts ostate.damage into dirty_staging
 *     3. Full-frame copy from wlroots buffer to framebuf (always full
 *        because pointer swap recycles buffers with stale data)
 *     4. send_frame() swaps framebuf pointer with send_buf, copies
 *        dirty_staging into dirty_tiles[buf]
 *     5. Send thread uses dirty_tiles to skip undamaged tiles entirely
 *        (no memory read). Damaged tiles are verified with tile_changed()
 *        to confirm they actually differ from prev_framebuf, since the
 *        headless backend reports full-screen damage on every frame
 *        and many "damaged" tiles may be pixel-identical.
 *
 *   The headless backend reports full-screen damage on every frame, so
 *   ostate.damage alone cannot detect idle screens.  The scene_dirty flag
 *   (set by toplevel/subsurface commit handlers) provides the actual
 *   idle detection, ensuring output_frame returns early when no client
 *   has submitted new content.
 *
 *   Fallback: when no damage info is available (scroll modified
 *   prev_framebuf, errors, or allocation failure), the send thread
 *   falls back to tile_changed() pixel comparison for all tiles.
 *
 * Pipelined I/O:
 *
 *   To maximize throughput, writes are pipelined:
 *
 *     1. Send thread calls p9_write_send() (non-blocking)
 *     2. drain_notify() signals drain thread
 *     3. Drain thread reads Rwrite response asynchronously
 *     4. Drain thread signals done_cond after each completed response to wake
 *        drain_throttle() and drain_pause() without polling
 *     5. Send thread continues with next batch
 *
 *   The drain_throttle() function prevents unbounded pipelining
 *   by waiting on done_cond when too many responses are pending.
 *   Similarly, drain_pause() waits on done_cond until all pending
 *   responses are drained. Both use condvar waits rather than
 *   polling, so they consume zero CPU while blocked.
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
 *        - If dirty tile bitmap is available and no scroll occurred,
 *          skip undamaged tiles without any memory read, then verify
 *          damaged tiles with tile_changed() to confirm actual changes
 *        - Otherwise fall back to comparing send_buf vs prev_framebuf
 *          for every tile via tile_changed()
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
 *        - 'v' flush command to display
 *
 *     8. State Update:
 *        - Copy sent tiles to prev_framebuf
 *        - Enable alpha-delta mode after first successful frame
 *
 * Padded Buffers:
 *
 *   The visible window dimensions are rounded UP to the nearest
 *   TILE_SIZE multiple, so every tile is a full 16×16 — no partial
 *   edge tiles.  The buffers (framebuf, send_buf[], prev_framebuf)
 *   are allocated at the padded size.  The compositor renders into
 *   the top-left visible_width × visible_height region; the right
 *   and bottom padding strips are zeroed by the send thread to keep
 *   edge-tile compression deterministic.
 *
 *     +----------------------+---+
 *     |                      | P |
 *     |   visible content    | A |
 *     |   (actual window)    | D |
 *     |                      |   |
 *     +----------------------+---+
 *     |     bottom padding       |
 *     +--------------------------+
 *
 *   The Plan 9 framebuffer image is also allocated at padded size.
 *   When copied to the screen image (window), the draw device clips
 *   the destination rectangle to the window bounds, so the padding
 *   pixels are never visible.  No border drawing is needed.
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
 *     - s->send_full
 *     - s->resize_pending
 *     - s->framebuf, s->send_buf[] pointer swaps
 *     - s->dirty_tiles[], s->dirty_valid[] (per-buffer bitmaps)
 *     - s->dirty_staging_valid (read and cleared by send_frame)
 *
 *   s->force_full_frame is accessed without send_lock by both the
 *   send thread and draw.c (relookup_window). It is a simple flag
 *   where a missed update results in at most one redundant full frame,
 *   so the race is benign.
 *
 *   s->send_cond is signaled by send_frame() (new frame) and by the
 *   mouse thread on resize notification ('r' from /dev/mouse) to wake
 *   the send thread immediately.
 *
 *   s->dirty_staging is written only by the output thread and read
 *   only under s->send_lock in send_frame(), so no additional
 *   synchronization is needed for the staging buffer itself.
 *
 *   drain.lock protects drain.cond and drain.done_cond:
 *     - drain.cond: signaled by drain_wake() (called from drain_notify
 *       and drain_resume) to wake the drain thread when work is available
 *     - drain.done_cond: broadcast by the drain thread after each
 *       completed response; waited on by drain_throttle() and
 *       drain_pause() to sleep until pending count drops
 *
 *   The drain thread uses atomic operations (via <stdatomic.h>) for
 *   its counters: pending, errors, running, paused. This allows the
 *   send thread to read these counters without acquiring drain.lock,
 *   reducing contention on the hot path.
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
 * Swaps the current framebuffer pointer (s->framebuf) with a free send
 * buffer and signals the send thread. This is a zero-copy handoff: the
 * send thread gets the just-rendered frame and the compositor gets a
 * recycled buffer for the next frame. Also copies the dirty tile bitmap
 * from staging (s->dirty_staging) into the per-buffer slot so the send
 * thread knows which tiles changed without pixel scanning.
 *
 * Buffer selection:
 *   - Finds a buffer that is neither pending nor active
 *   - If no buffer is free, the frame is dropped (throttling)
 *
 * Flags copied:
 *   - If s->force_full_frame is set, s->send_full is set
 *   - If s->dirty_staging_valid is set, dirty_tiles[buf] is populated
 *
 * This function returns immediately after the swap; actual transmission
 * happens asynchronously in the send thread.
 *
 * Thread-safe: acquires s->send_lock internally.
 *
 * s: server state containing framebuffer and send thread context
 *
 * Preconditions:
 *   - s->framebuf must be valid
 *   - framebuf and send_buf[] must be allocated at the same padded size
 *     (width × height × 4 bytes) since their pointers are swapped
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
 *      (signaled by send_frame and mouse thread on resize)
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

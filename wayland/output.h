/*
 * output.h - Output and input device management
 *
 * This module handles Wayland output (display) creation and the frame
 * rendering loop, as well as input device attachment.
 *
 * Output Handling:
 *
 *   new_output() is called when wlroots creates a new output. For p9wl,
 *   there is typically one headless output sized to match the Plan 9
 *   window. The output is configured with:
 *
 *     - Custom mode matching the Plan 9 window dimensions
 *     - 60Hz refresh rate (60000 mHz)
 *     - Optional HiDPI scale factor (if s->scale > 1.0)
 *     - Scene graph output for rendering
 *
 * Frame Loop:
 *
 *   The output_frame handler (internal) runs the compositor's render loop:
 *
 *     1. Check for pending resize from mouse thread (s->resize_pending)
 *     2. If resize pending:
 *        a. Reallocate host buffers (framebuf, prev_framebuf, send_buf)
 *        b. Reallocate dirty tile bitmaps (dirty_tiles, dirty_staging)
 *        c. Update s->width, s->height, s->tiles_x, s->tiles_y
 *        d. Reallocate Plan 9 images via reallocate_draw_images()
 *        e. Resize wlroots output and reconfigure all toplevels
 *        f. Set force_full_frame and scene_dirty flags
 *     3. Throttle frames if FRAME_INTERVAL_MS is non-zero
 *     4. Check scene_dirty and force_full_frame flags.  If both are
 *        clear, send frame_done and return immediately — skipping
 *        build_state, buffer copy, and send_frame.  This is the
 *        primary idle-screen optimization.  scene_dirty is set by
 *        client commits; force_full_frame is set by resize handling
 *        (step 2) and error recovery.
 *     5. Build scene output state via wlr_scene_output_build_state()
 *     6. Extract compositor damage into dirty tile staging bitmap
 *     7. Full-frame copy from rendered buffer to s->framebuf.
 *        (Partial copy was removed: pointer swap between framebuf and
 *        send_buf means the recycled buffer has stale data, so all
 *        rows must be written.  The scene_dirty gate in step 4
 *        ensures this only runs when content actually changed.)
 *     8. Commit output state and send frame done
 *     9. Trigger send_frame() when there is actual work
 *
 * Damage-Based Dirty Tile Tracking:
 *
 *   The headless backend reports full-screen damage on every frame,
 *   making ostate.damage useless for idle detection.  Instead, idle
 *   frames are skipped via the scene_dirty flag (step 4 above).
 *
 *   When rendering does occur, ostate.damage is still extracted into
 *   a per-tile bitmap (s->dirty_staging) and used to tell the send
 *   thread which tiles to compress and transmit.  The framebuf copy
 *   is always full-frame (required because pointer swap between
 *   framebuf and send_buf leaves the recycled buffer with stale data).
 *
 *   The send thread trusts the damage bitmap as ground truth: undamaged
 *   tiles are skipped without pixel comparison, damaged tiles are
 *   assumed changed.
 *
 *   Fallback: if damage extraction fails (allocation error),
 *   dirty_staging_valid remains 0 and the send thread falls back to
 *   pixel comparison via tile_changed().
 *
 * Image Reallocation:
 *
 *   reallocate_draw_images() (internal) handles Plan 9 image resize:
 *     - Frees old framebuffer and delta images via 'f' command
 *     - Allocates new images at new dimensions via 'b' command
 *     - Uses alloc_image_cmd/free_image_cmd helpers from draw_cmd.h
 *
 * Input Device Handling:
 *
 *   new_input() attaches pointer devices to the cursor. Keyboard input
 *   comes from Plan 9 via the input threads, not from Wayland input
 *   devices, so keyboard devices are ignored.
 *
 * Usage:
 *
 *   Wire up handlers during server initialization:
 *
 *     server->new_output.notify = new_output;
 *     wl_signal_add(&backend->events.new_output, &server->new_output);
 *
 *     server->new_input.notify = new_input;
 *     wl_signal_add(&backend->events.new_input, &server->new_input);
 */

#ifndef P9WL_OUTPUT_H
#define P9WL_OUTPUT_H

#include "../types.h"

/* ============== Output Handling ============== */

/*
 * Handle new output creation.
 *
 * Initializes rendering for the output, sets the custom mode to match
 * the Plan 9 window size, adds it to the output layout, and sets up
 * frame and destroy listeners.
 *
 * For HiDPI displays (scale > 1.0), configures the output scale and
 * logs both physical and logical dimensions.
 *
 * Stores output reference in s->output and creates s->scene_output.
 *
 * l: wl_listener from server->new_output
 * d: wlr_output pointer
 */
void new_output(struct wl_listener *l, void *d);

/* ============== Input Device Handling ============== */

/*
 * Handle new input device.
 *
 * For pointer devices (WLR_INPUT_DEVICE_POINTER), attaches them to
 * the server's cursor via wlr_cursor_attach_input_device() so that
 * cursor motion events are properly aggregated.
 *
 * Keyboard and other device types are currently ignored since input
 * comes from Plan 9 rather than local devices.
 *
 * l: wl_listener from server->new_input
 * d: wlr_input_device pointer
 */
void new_input(struct wl_listener *l, void *d);

#endif /* P9WL_OUTPUT_H */

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
 *        f. Set force_full_frame flag
 *     3. Throttle frames based on FRAME_INTERVAL_MS if defined
 *     4. Build scene output state via wlr_scene_output_build_state()
 *     5. Copy rendered pixels from buffer to s->framebuf
 *     6. Extract compositor damage (ostate.damage) into dirty tile
 *        staging bitmap (s->dirty_staging) for the send thread
 *     7. Commit output state and send frame done
 *     8. Trigger send_frame() only when there is actual work:
 *        damage rects found, force_full_frame set, or no damage
 *        info available (fallback).  When the screen is idle and
 *        damage tracking reports zero rects, send_frame() is
 *        skipped entirely so the send thread stays asleep.
 *
 * Damage-Based Dirty Tile Tracking:
 *
 *   wlr_scene_output_build_state() tracks which pixels changed via
 *   ostate.damage (a pixman region). The output_frame handler reads
 *   this damage unconditionally (not gated on the WLR_OUTPUT_STATE_DAMAGE
 *   committed flag, which tracks caller-set fields, not scene builder
 *   output). The damage rectangles are converted to a per-tile bitmap
 *   (s->dirty_staging). send_frame() copies this bitmap into the
 *   per-send-buffer slot (s->dirty_tiles[buf]) under the send lock.
 *
 *   The send thread trusts the damage bitmap as ground truth: undamaged
 *   tiles are skipped without any pixel comparison, and damaged tiles
 *   are assumed changed. When damage reports zero rects (idle screen),
 *   send_frame() is not called and the send thread stays asleep.
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

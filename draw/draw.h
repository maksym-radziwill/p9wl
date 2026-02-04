/*
 * draw.h - /dev/draw initialization and rio window management
 *
 * Manages the connection to /dev/draw for rendering frames to a Plan 9
 * display server, and handles rio window lookup, geometry tracking, and
 * resize detection.
 *
 * Draw Device Files:
 *
 *   /dev/draw/new       - Open to get client ID and screen geometry
 *   /dev/draw/<id>/data - Send draw commands (alloc, load, composite)
 *   /dev/draw/<id>/ctl  - Read current geometry (12x12 ASCII fields)
 *   /dev/winname        - Read current rio window name
 *
 * Initialization Sequence (init_draw):
 *
 *   1. Open /dev/draw/new, read client ID and initial screen rect
 *   2. Open /dev/draw/<id>/data and /dev/draw/<id>/ctl
 *   3. Open /dev/winname if present (rio environment)
 *   4. Look up window by name via 'n' draw command
 *   5. Read actual window geometry from ctl
 *   6. Resize window via /dev/wctl to tile-aligned dimensions
 *      (with RIO_BORDER padding to preserve the yellow border)
 *   7. Allocate draw images (see below)
 *
 *   If no rio window is found (no /dev/winname or lookup fails),
 *   screen_id is set to 0 and compositing targets the root screen.
 *
 * Image Allocation:
 *
 *   image_id (1):  Framebuffer (XRGB32, full window size)
 *                  Target for tile loads; composited to screen_id
 *
 *   opaque_id (2): Opaque mask (1x1 white, GREY1, replicated)
 *                  Used as mask for opaque draw operations
 *
 *   delta_id (5):  Delta buffer (ARGB32, full window size)
 *                  Sparse alpha-composited updates (see compress.h)
 *
 * Dimension Alignment:
 *
 *   Window dimensions are rounded down to tile boundaries (TILE_SIZE)
 *   with a minimum of 4 tiles per axis. This ensures all tiles are
 *   complete and simplifies change detection and compression.
 *
 *   After alignment, the window is resized via /dev/wctl to exactly
 *   aligned_width + 2*RIO_BORDER by aligned_height + 2*RIO_BORDER.
 *   The centering logic in parse_ctl naturally places content at
 *   (rminx + RIO_BORDER, rminy + RIO_BORDER), leaving the border
 *   pixels untouched by the framebuffer.
 *
 *   If the resize fails (no rio, or wctl not writable), centering
 *   within the original window rect is used as fallback.
 *
 * Window Relookup (relookup_window):
 *
 *   Called when the window may have moved or resized (triggered by
 *   'r' event from /dev/mouse via the mouse thread).
 *
 *   1. Re-read /dev/winname for current window name
 *   2. Free old window reference via 'f' draw command
 *   3. Re-lookup window by name via 'n' draw command
 *   4. Read geometry from ctl, resize if needed
 *   5. Update draw state with new position/dimensions
 *
 *   If the window size changed:
 *     - Sets s->resize_pending = 1
 *     - Stores new dimensions in s->pending_width/height
 *     - The send thread detects this and triggers reallocation
 *
 *   If only position changed (same size):
 *     - Sets s->force_full_frame = 1 (full redraw at new position)
 *     - Sets s->frame_dirty = 1
 *
 * Thread Safety:
 *
 *   init_draw() is called once at startup, before other threads.
 *   relookup_window() is called from the send thread under send_lock
 *   (triggered by s->window_changed flag set by the mouse thread).
 *   delete_rio_window() is called during shutdown after threads join.
 */

#ifndef P9WL_DRAW_H
#define P9WL_DRAW_H

#include "../types.h"

/*
 * Initialize the draw device connection.
 *
 * Opens /dev/draw, looks up the rio window, resizes to aligned
 * dimensions, and allocates framebuffer, mask, and delta images.
 *
 * s->p9_draw must be connected before calling.
 * On success, s->draw is fully populated.
 *
 * Returns 0 on success, -1 on failure.
 */
int init_draw(struct server *s);

/*
 * Re-lookup the current window after move/resize.
 *
 * Re-reads winname, frees old window reference, re-lookups by name,
 * resizes to aligned dimensions, and updates geometry. Sets either
 * s->resize_pending or s->force_full_frame depending on whether
 * the size changed.
 *
 * Returns 0 on success, -1 on failure.
 */
int relookup_window(struct server *s);

/*
 * Delete the rio window via /dev/wctl.
 *
 * Sends "delete" to wctl. Silently ignores errors (window may
 * already be gone during shutdown).
 */
void delete_rio_window(struct p9conn *p9);

#endif /* P9WL_DRAW_H */

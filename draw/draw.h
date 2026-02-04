/*
 * draw.h - Plan 9 draw device initialization and window management
 *
 * Manages connection to /dev/draw for rendering frames to a Plan 9
 * display server. Handles:
 *   - Opening and configuring draw device files
 *   - Allocating framebuffer and auxiliary images
 *   - Window lookup by name (for rio window manager)
 *   - Geometry tracking and resize detection
 *
 * The draw device uses a client-server model where this code acts
 * as a client, sending draw commands via 9P to the server.
 *
 * Draw Device Files:
 *
 *   /dev/draw/new      - Open to get client ID and screen geometry
 *   /dev/draw/<id>/data - Send draw commands
 *   /dev/draw/<id>/ctl  - Read current geometry
 *   /dev/winname       - Read current window name (rio)
 *
 * Image Allocation:
 *
 *   init_draw() allocates these Plan 9 images:
 *
 *     image_id (1):  Framebuffer image (XRGB32, full window size)
 *                    Used for rendering compositor output
 *
 *     opaque_id (2): Opaque mask (1x1 white, GREY1, replicated)
 *                    Used as mask for opaque draw operations
 *
 *     border_id (4): Border color (1x1 pale gray, ARGB32, replicated)
 *                    Used for window border drawing
 *
 *     delta_id (5):  Delta image (ARGB32, full window size)
 *                    Used for sparse alpha-composited updates
 *
 * Dimension Alignment:
 *
 *   Window dimensions are aligned to tile boundaries (TILE_SIZE) with
 *   a minimum of 4 tiles (MIN_ALIGNED_DIM = TILE_SIZE * 4). Content is
 *   centered within the actual window bounds when dimensions don't
 *   align perfectly.
 *
 *   Helpers (internal):
 *     align_dimension()     - Round down to tile boundary with minimum
 *     center_in_window()    - Compute centered position
 *     parse_ctl_geometry()  - Parse ctl file and compute all values
 *     resize_to_aligned()   - Resize rio window via /dev/wctl to match
 *                             aligned dimensions exactly
 */

#ifndef P9WL_DRAW_H
#define P9WL_DRAW_H

#include "../types.h"

/*
 * Initialize the draw device connection.
 *
 * Opens /dev/draw/new to establish a draw client, then:
 *   1. Reads client ID and initial screen geometry
 *   2. Opens /dev/draw/<id>/data for sending commands
 *   3. Opens /dev/draw/<id>/ctl for reading geometry
 *   4. Opens /dev/winname if available (rio window manager)
 *   5. Looks up window by name using 'n' command
 *   6. Reads actual window geometry from ctl
 *   7. Resizes window via /dev/wctl to aligned dimensions
 *      (eliminates centering offset; falls back to centering
 *      if resize is not supported)
 *   8. Allocates required images:
 *      - Framebuffer image (XRGB32)
 *      - Opaque mask (1x1 white, replicated)
 *      - Border color (1x1 gray, replicated)
 *      - Delta image (ARGB32, for sparse updates)
 *
 * s: server state, must have p9_draw connection established
 *
 * Returns 0 on success, -1 on failure.
 * On success, s->draw is populated with image IDs, fids, and geometry.
 */
int init_draw(struct server *s);

/*
 * Re-lookup the current window after a change.
 *
 * Called when the window may have moved or resized (e.g., after
 * receiving a window change notification from wctl thread).
 *
 * Performs:
 *   1. Re-reads /dev/winname for current window name
 *   2. Frees old window reference via 'f' command
 *   3. Re-lookups window by name via 'n' command
 *   4. Re-reads ctl for new geometry
 *   5. Resizes window via /dev/wctl to aligned dimensions
 *   6. Re-reads ctl for post-resize geometry
 *   7. Updates draw state with new position/dimensions
 *
 * If the window size changed:
 *   - Sets s->resize_pending = 1
 *   - Stores new dimensions in s->pending_width/height
 *   - Stores new position in s->pending_minx/miny
 *
 * If only position changed (same size):
 *   - Updates draw->win_minx/miny
 *   - Sets s->force_full_frame = 1 to trigger complete redraw
 *   - Sets s->frame_dirty = 1
 *
 * s: server state with active draw connection
 *
 * Returns 0 on success, -1 on failure.
 */
int relookup_window(struct server *s);

/*
 * Delete the rio window.
 *
 * Sends "delete" command to /dev/wctl to close the rio window.
 * Called during shutdown to clean up the window manager state.
 *
 * Opens /dev/wctl, writes "delete", and returns. Errors during
 * walk or open are silently ignored (window may already be gone).
 *
 * p9: 9P connection to use (typically s->p9_draw)
 */
void delete_rio_window(struct p9conn *p9);

#endif /* P9WL_DRAW_H */

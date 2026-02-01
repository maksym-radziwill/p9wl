/*
 * draw.h - Plan 9 draw device initialization and window management
 *
 * Manages connection to /dev/draw for rendering frames to a Plan 9
 * display server. Handles:
 * - Opening and configuring draw device files
 * - Allocating framebuffer and auxiliary images
 * - Window lookup by name (for rio window manager)
 * - Geometry tracking and resize detection
 *
 * The draw device uses a client-server model where this code acts
 * as a client, sending draw commands via 9P to the server.
 */

#ifndef P9WL_DRAW_H
#define P9WL_DRAW_H

#include "../types.h"

/*
 * Initialize the draw device connection.
 *
 * Opens /dev/draw/new to establish a draw client, then:
 * 1. Reads client ID and initial screen geometry
 * 2. Opens /dev/draw/<id>/data for sending commands
 * 3. Opens /dev/draw/<id>/ctl for reading geometry
 * 4. Looks up window by name (if /dev/winname exists)
 * 5. Allocates required images:
 *    - Framebuffer image (XRGB32)
 *    - Opaque mask (1x1 white, replicated)
 *    - Border color (1x1 gray, replicated)
 *    - Delta image (ARGB32, for sparse updates)
 *
 * s: server state, must have p9_draw connection established
 *
 * Returns 0 on success, -1 on failure.
 * On success, s->draw is populated with image IDs and geometry.
 */
int init_draw(struct server *s);

/*
 * Re-lookup the current window after a change.
 *
 * Called when the window may have moved or resized (e.g., after
 * receiving a window change notification). Re-reads /dev/winname,
 * frees the old window reference, looks up the new window, and
 * updates geometry.
 *
 * If the window size changed, sets s->resize_pending and stores
 * new dimensions in s->pending_width/height.
 *
 * If only position changed, sets s->force_full_frame to trigger
 * a complete redraw at the new location.
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
 * p9: 9P connection to use (typically s->p9_draw)
 */
void delete_rio_window(struct p9conn *p9);

#endif /* P9WL_DRAW_H */

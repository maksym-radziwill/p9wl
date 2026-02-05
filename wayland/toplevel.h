/*
 * toplevel.h - XDG toplevel window management
 *
 * Manages the lifecycle of XDG toplevel surfaces (application windows)
 * and their subsurfaces.
 *
 * Toplevel Lifecycle:
 *
 *   new_toplevel()      - Creates scene tree at (0,0), adds to
 *                         server's toplevel list, sets up listeners.
 *   toplevel_commit()   - Initial: configures maximized at logical
 *                         dimensions. Subsequent: tracks map/unmap,
 *                         scans for new subsurfaces, marks dirty tiles.
 *   toplevel_destroy()  - Cleans up focus, subsurfaces, listeners.
 *                         Initiates shutdown if last toplevel.
 *
 * Subsurface Tracking:
 *
 *   Subsurfaces are discovered by scanning the surface's below/above
 *   lists on each commit. Each tracked subsurface gets commit/destroy
 *   listeners for map/unmap detection, dirty tile marking, and focus
 *   rechecks. Cleaned up on toplevel destruction.
 */

#ifndef P9WL_TOPLEVEL_H
#define P9WL_TOPLEVEL_H

#include "../types.h"

/*
 * Handle new XDG toplevel creation.
 *
 * Creates scene tree, initializes toplevel, adds to server list.
 * Configured to fill the output at logical dimensions on initial commit.
 *
 * l: wl_listener from server->new_xdg_toplevel
 * d: wlr_xdg_toplevel pointer
 */
void new_toplevel(struct wl_listener *l, void *d);

#endif /* P9WL_TOPLEVEL_H */

/*
 * toplevel.h - XDG toplevel window management
 *
 * This module handles the lifecycle of XDG toplevel surfaces (application
 * windows). Each toplevel is tracked with a struct toplevel that contains
 * scene graph nodes, surface references, and subsurface tracking.
 *
 * Toplevel Lifecycle:
 *
 *   1. new_toplevel() - Called when client creates xdg_toplevel
 *      - Allocates toplevel struct
 *      - Creates scene graph nodes
 *      - Sends initial configure (size, maximized, activated)
 *
 *   2. toplevel_commit() - Called on each surface commit
 *      - Tracks map/unmap state transitions
 *      - Monitors for new subsurfaces
 *      - Triggers focus updates
 *
 *   3. toplevel_destroy() - Called when toplevel is destroyed
 *      - Cleans up subsurface tracking
 *      - Removes from server's toplevel list
 *      - Initiates compositor shutdown if last toplevel
 *
 * Subsurface Tracking:
 *
 *   Wayland subsurfaces are child surfaces that move with their parent.
 *   This module tracks them via subsurface_track structs to ensure proper
 *   focus handling and cleanup. Subsurfaces are discovered by scanning
 *   the surface's subsurface lists on each commit.
 *
 * Usage:
 *
 *   Wire up new_toplevel as the handler for xdg_shell's new_toplevel event:
 *
 *     server->new_xdg_toplevel.notify = new_toplevel;
 *     wl_signal_add(&xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
 */

#ifndef P9WL_TOPLEVEL_H
#define P9WL_TOPLEVEL_H

#include "../types.h"

/*
 * Handle new XDG toplevel creation.
 *
 * Creates the scene tree, initializes the toplevel struct, adds it to
 * the server's toplevel list, and sets up commit/destroy listeners.
 *
 * The toplevel is configured to fill the entire output at the current
 * logical dimensions, with maximized and activated states set.
 */
void new_toplevel(struct wl_listener *l, void *d);

#endif /* P9WL_TOPLEVEL_H */

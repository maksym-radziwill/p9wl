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
 *      - Sets s->has_toplevel and s->had_toplevel flags
 *      - Allocates toplevel struct
 *      - Creates scene tree via wlr_scene_xdg_surface_create()
 *      - Stores scene_tree in xdg->base->data for cross-referencing
 *      - Stores toplevel pointer in scene_tree->node.data
 *      - Positions scene node at (0,0)
 *      - Initializes empty subsurface tracking list
 *      - Adds to server's toplevel list
 *      - Sets up commit and destroy listeners
 *
 *   2. toplevel_commit() - Called on each surface commit
 *      - Initial commit: sends configure with logical dimensions,
 *        maximized and activated states via wlr_xdg_toplevel_set_*()
 *      - Tracks map/unmap state transitions via wlr_surface_has_buffer()
 *      - On map: calls focus_on_surface_map(), updates pointer focus
 *      - On unmap: calls focus_on_surface_unmap()
 *      - Scans for new subsurfaces via check_new_subsurfaces()
 *      - Sets scene_dirty flag and schedules output frame
 *
 *   3. toplevel_destroy() - Called when toplevel is destroyed
 *      - Calls focus_on_surface_destroy() to clean up focus state
 *      - Cleans up all tracked subsurfaces
 *      - Removes from server's toplevel list
 *      - Frees toplevel struct
 *      - If last toplevel (s->had_toplevel && list empty):
 *        * Sets s->running = 0 and joins send_thread
 *        * Deletes rio window via delete_rio_window()
 *        * Disconnects p9_draw and calls exit(0)
 *
 * Subsurface Tracking:
 *
 *   Wayland subsurfaces are child surfaces that move with their parent.
 *   This module tracks them via subsurface_track structs to ensure proper
 *   focus handling and cleanup. Subsurfaces are discovered by scanning
 *   the surface's subsurface lists on each commit.
 *
 *   The FOR_EACH_SUBSURFACE macro iterates both below and above lists:
 *
 *     FOR_EACH_SUBSURFACE(surface, sub) { ... }
 *
 *   subsurface_track contains:
 *     - subsurface: wlr_subsurface pointer
 *     - server: back-reference for focus operations
 *     - toplevel: owning toplevel
 *     - mapped: current visibility state
 *     - destroy/commit: Wayland listeners
 *     - link: position in toplevel's subsurface list
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

/* ============== Toplevel Handling ============== */

/*
 * Handle new XDG toplevel creation.
 *
 * Creates the scene tree, initializes the toplevel struct, adds it to
 * the server's toplevel list, and sets up commit/destroy listeners.
 *
 * The toplevel is configured to fill the entire output at the current
 * logical dimensions (s->width/s->scale by s->height/s->scale), with
 * maximized and activated states set on initial commit.
 *
 * Scene tree node is positioned at (0,0) - all toplevels fill the
 * entire window in this compositor.
 *
 * l: wl_listener from server->new_xdg_toplevel
 * d: wlr_xdg_toplevel pointer
 */
void new_toplevel(struct wl_listener *l, void *d);

#endif /* P9WL_TOPLEVEL_H */

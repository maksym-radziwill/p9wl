/*
 * popup.h - XDG popup lifecycle and subsurface tracking
 *
 * Handles creation, commit, and destruction of XDG popups (menus,
 * dropdowns, tooltips) and tracks their subsurfaces. Focus management
 * is delegated to focus_manager.c.
 *
 * Popup Lifecycle:
 *
 *   new_popup()      - Validates parent, creates scene tree, registers
 *                      with focus manager, sets up listeners.
 *   popup_commit()   - Initial: unconstrains to screen bounds.
 *                      Subsequent: tracks map/unmap, scans for new
 *                      subsurfaces, marks scene dirty.
 *   popup_destroy()  - Unregisters from focus manager, cleans up
 *                      subsurface tracking, removes listeners.
 *
 * Subsurface Tracking:
 *
 *   Popup surfaces can have subsurfaces (child widgets, embedded
 *   content). These are discovered on each commit by scanning the
 *   surface's subsurface lists and tracked with popup_sub structs
 *   linked into popup_data.subsurfaces.
 *
 *   Each tracked subsurface gets commit/destroy listeners for
 *   map/unmap detection and focus rechecks.
 *
 * Required popup_data fields (defined in focus_manager.h or types.h):
 *
 *   struct wl_list subsurfaces;   // list of popup_sub via .link
 *
 *   The grab listener and grab.link field are removed — has_grab
 *   (bool) is sufficient for the focus manager.
 */

#ifndef P9WL_POPUP_H
#define P9WL_POPUP_H

#include "../types.h"

/*
 * Handle new XDG popup creation.
 *
 * Creates scene graph node as child of parent's scene tree, allocates
 * popup_data, registers with focus manager, and sets up listeners.
 * Parent must be a valid XDG surface with data set.
 *
 * l: wl_listener from server->new_xdg_popup
 * d: wlr_xdg_popup pointer
 */
void new_popup(struct wl_listener *l, void *d);

#endif /* P9WL_POPUP_H */

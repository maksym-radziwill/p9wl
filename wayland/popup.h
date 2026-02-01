/*
 * popup.h - XDG popup lifecycle management
 *
 * This module handles the creation and destruction of XDG popups (menus,
 * dropdowns, tooltips). Focus management for popups is handled by the
 * focus_manager module.
 *
 * Popup Lifecycle:
 *
 *   1. new_popup() - Called when client creates xdg_popup
 *      - Validates parent surface (must be valid XDG surface with data)
 *      - Creates scene tree as child of parent's scene tree
 *      - Allocates and initializes popup_data struct
 *      - Registers popup with focus manager via focus_popup_register()
 *      - Sets up commit and destroy listeners
 *      - Detects grab state from popup->seat (non-NULL = has grab)
 *
 *   2. popup_commit() - Called on each surface commit
 *      - Initial commit: unconstrains popup to screen bounds using
 *        logical dimensions (physical / scale)
 *      - Subsequent commits: tracks map/unmap state transitions
 *      - On map: calls focus_popup_mapped() to handle focus
 *      - On unmap: calls focus_popup_unmapped() to restore focus
 *
 *   3. popup_destroy() - Called when popup is destroyed
 *      - Unregisters from focus manager via focus_popup_unregister()
 *      - Removes commit, destroy, and grab listeners
 *      - Frees popup_data struct
 *
 * Data Structures:
 *
 *   The popup_data struct (defined in focus_manager.h) tracks:
 *     - popup: wlr_xdg_popup pointer
 *     - surface: wlr_surface pointer for quick access
 *     - scene_tree: scene graph node for rendering
 *     - server: back-reference to server state
 *     - configured: set after initial configure sent
 *     - commit_count: number of commits received (for debugging)
 *     - mapped: true when surface has buffer and is visible
 *     - has_grab: true if popup requested keyboard grab (menus)
 *     - link: position in focus_manager's popup_stack
 *
 * Usage:
 *
 *   Wire up new_popup as the handler for xdg_shell's new_popup event:
 *
 *     server->new_xdg_popup.notify = new_popup;
 *     wl_signal_add(&xdg_shell->events.new_popup, &server->new_xdg_popup);
 */

#ifndef P9WL_POPUP_H
#define P9WL_POPUP_H

#include "../types.h"

/* ============== Popup Handling ============== */

/*
 * Handle new XDG popup creation.
 *
 * Creates scene graph nodes, allocates popup_data, registers with the
 * focus manager, and sets up commit/destroy listeners.
 *
 * The popup's parent must be a valid XDG surface with data set
 * (parent->data points to parent's scene tree). Returns early with
 * error log if parent is invalid.
 *
 * Scene tree is created via wlr_scene_xdg_surface_create() as a child
 * of the parent's scene tree, ensuring proper layering.
 *
 * l: wl_listener from server->new_xdg_popup
 * d: wlr_xdg_popup pointer
 */
void new_popup(struct wl_listener *l, void *d);

#endif /* P9WL_POPUP_H */

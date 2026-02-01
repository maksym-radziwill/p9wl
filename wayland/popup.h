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
 *   2. popup_commit() - Called on each surface commit
 *      - Initial commit: unconstrain popup to screen bounds
 *      - Subsequent commits: track map/unmap state
 *   3. popup_destroy() - Called when popup is destroyed
 *
 * The popup_data struct (defined in focus_manager.h) tracks each popup's
 * state and position in the focus manager's grab stack.
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

/*
 * Handle new XDG popup creation.
 *
 * Creates scene graph nodes, allocates popup_data, registers with the
 * focus manager, and sets up commit/destroy listeners.
 *
 * The popup's parent must be a valid XDG surface with data set.
 */
void new_popup(struct wl_listener *l, void *d);

#endif /* P9WL_POPUP_H */

/*
 * popup.h - Popup management and handlers
 *
 * NOTE: struct popup_data is now defined in focus_manager.h
 * This header provides the legacy API for popup management.
 * New code should use the focus_manager API directly.
 */

#ifndef P9WL_POPUP_H
#define P9WL_POPUP_H

#include <stdbool.h>
#include "../types.h"  /* Includes focus_manager.h which defines struct popup_data */

/*
 * Legacy API - these functions delegate to focus_manager.
 * Kept for backward compatibility.
 */

/* Get topmost popup from stack */
struct popup_data *get_topmost_popup(struct server *s);

/* Find popup containing a surface */
struct popup_data *find_popup_by_surface(struct server *s, struct wlr_surface *surface);

/* Dismiss all popups */
void dismiss_all_popups(struct server *s);

/* Dismiss topmost grabbed popup (e.g., on Escape key) */
bool dismiss_topmost_grabbed_popup(struct server *s);

/* Handler for new popup creation */
void new_popup(struct wl_listener *l, void *d);

#endif /* P9WL_POPUP_H */

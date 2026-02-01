/*
 * focus.h - Focus management and surface lookup
 *
 * NOTE: This is now a legacy compatibility header.
 * The actual implementation has been consolidated in focus_manager.h/c
 * 
 * New code should use the focus_manager API directly:
 *   focus_toplevel_from_surface()  -> fm_focus_toplevel()
 *   surface_at_cursor()            -> focus_surface_at_cursor()
 *   toplevel_at_cursor()           -> focus_toplevel_at_cursor()
 *   focus_toplevel()               -> fm_focus_toplevel()
 *   recheck_pointer_focus()        -> focus_pointer_recheck()
 */

#ifndef P9WL_FOCUS_H
#define P9WL_FOCUS_H

#include "../types.h"  /* Includes focus_manager.h */

/*
 * Legacy API - these functions delegate to focus_manager.
 * Kept for backward compatibility with existing code.
 */

/* Find toplevel from a surface (including subsurfaces) */
struct toplevel *toplevel_from_surface(struct server *s, struct wlr_surface *surface);

/* Find toplevel from a scene node */
struct toplevel *toplevel_from_node(struct wlr_scene_node *node);

/* Get surface under cursor position */
struct wlr_surface *surface_at_cursor(struct server *s, double *out_sx, double *out_sy);

/* Get toplevel under cursor position */
struct toplevel *toplevel_at_cursor(struct server *s);

/* Focus a toplevel (raise, activate, keyboard focus) */
void focus_toplevel(struct server *s, struct toplevel *tl);

/* Recheck pointer focus after surface changes */
void recheck_pointer_focus(struct server *s);

#endif /* P9WL_FOCUS_H */

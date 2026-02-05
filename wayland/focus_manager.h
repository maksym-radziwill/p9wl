/*
 * focus_manager.h - Pointer focus, keyboard focus, popup grab stack.
 *
 * Tracks two independent focus targets: pointer (where mouse events go)
 * and keyboard (where key events go). Popups form a grab stack that can
 * capture keyboard focus. Pointer focus is deferred while buttons are
 * held to prevent focus changes mid-drag.
 *
 * The wlr_seat is the single source of truth for current focus surfaces.
 * This module just wraps the seat calls with deferral and popup logic.
 */

#ifndef FOCUS_MANAGER_H
#define FOCUS_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct server;
struct toplevel;
struct wlr_surface;
struct wlr_xdg_popup;

/* ============== Data Structures ============== */

struct popup_data {
    struct wlr_xdg_popup *popup;
    struct wlr_surface *surface;
    struct wlr_scene_tree *scene_tree;
    struct server *server;

    int configured;
    int commit_count;
    bool mapped;
    bool has_grab;

    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_list subsurfaces;
    struct wl_list link;            /* focus_manager.popup_stack */
};

struct focus_manager {
    struct server *server;

    struct wl_list popup_stack;     /* popup_data.link, most recent first */

    bool pointer_deferred;          /* focus change waiting for button release */
    uint32_t modifier_state;
    int focus_change_count;         /* debug */
};

/* ============== Init / Cleanup ============== */

void focus_manager_init(struct focus_manager *fm, struct server *server);
void focus_manager_cleanup(struct focus_manager *fm);

/* ============== Queries ============== */

/* Hit test at cursor position. Returns surface and surface-local coords.
 * Falls back to first mapped toplevel if nothing is directly under cursor. */
struct wlr_surface *focus_surface_at_cursor(struct focus_manager *fm,
                                            double *sx, double *sy);

/* Walk subsurface parents, then scan toplevel list. */
struct toplevel *focus_toplevel_from_surface(struct focus_manager *fm,
                                             struct wlr_surface *surface);

struct toplevel *focus_toplevel_at_cursor(struct focus_manager *fm);

/* ============== Pointer Focus ============== */

/* Set pointer focus. If force=false and buttons are held, defers until release.
 * Pass NULL to clear. */
void focus_pointer_set(struct focus_manager *fm, struct wlr_surface *surface,
                       double sx, double sy, bool force);

void focus_pointer_motion(struct focus_manager *fm, double sx, double sy,
                          uint32_t time_msec);

/* Fresh hit test + update. No-op while buttons held. */
void focus_pointer_recheck(struct focus_manager *fm);

/* Call when last button released — flushes deferred focus. */
void focus_pointer_button_released(struct focus_manager *fm);

/* ============== Keyboard Focus ============== */

void focus_keyboard_set(struct focus_manager *fm, struct wlr_surface *surface);
void focus_keyboard_set_modifiers(struct focus_manager *fm, uint32_t modifiers);
uint32_t focus_keyboard_get_modifiers(struct focus_manager *fm);

/* ============== Toplevel Focus ============== */

/* Deactivate previous, raise + activate tl, set keyboard focus. */
void focus_toplevel(struct focus_manager *fm, struct toplevel *tl);
struct toplevel *focus_get_focused_toplevel(struct focus_manager *fm);

/* ============== Popup Stack ============== */

void focus_popup_register(struct focus_manager *fm, struct popup_data *pd);
void focus_popup_unregister(struct focus_manager *fm, struct popup_data *pd);
void focus_popup_mapped(struct focus_manager *fm, struct popup_data *pd);
void focus_popup_unmapped(struct focus_manager *fm, struct popup_data *pd);

struct popup_data *focus_popup_get_topmost(struct focus_manager *fm);
struct popup_data *focus_popup_from_surface(struct focus_manager *fm,
                                             struct wlr_surface *surface);

void focus_popup_dismiss_all(struct focus_manager *fm);
bool focus_popup_dismiss_topmost_grabbed(struct focus_manager *fm);
bool focus_popup_stack_empty(struct focus_manager *fm);

/* ============== Surface Lifecycle ============== */

void focus_on_surface_map(struct focus_manager *fm, struct wlr_surface *surface,
                          bool is_toplevel);
void focus_on_surface_unmap(struct focus_manager *fm, struct wlr_surface *surface);
void focus_on_surface_destroy(struct focus_manager *fm, struct wlr_surface *surface);

/* ============== Click Handling ============== */

/* Dismiss popups if needed, focus toplevel, return surface for button event. */
struct wlr_surface *focus_handle_click(struct focus_manager *fm,
                                        struct wlr_surface *clicked,
                                        double sx, double sy, uint32_t button);

/* ============== Coordinate Conversion ============== */

static inline int focus_phys_to_logical(int phys, float scale) {
    return (int)(phys / scale + 0.5f);
}

static inline int focus_logical_to_phys(int logical, float scale) {
    return (int)(logical * scale + 0.5f);
}

#endif /* FOCUS_MANAGER_H */

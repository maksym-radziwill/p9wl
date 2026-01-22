/*
 * focus_manager.h - Unified focus state machine
 *
 * Consolidates all focus-related logic:
 * - Pointer focus (enter/leave/motion)
 * - Keyboard focus (surface targeting)
 * - Popup grab stack management
 * - Focus transitions on map/unmap/destroy
 *
 * The focus manager maintains two independent focus targets:
 * 1. Pointer focus - which surface receives pointer events
 * 2. Keyboard focus - which surface receives key events
 *
 * Popup grabs temporarily redirect keyboard focus while active.
 *
 * NOTE: This header is included by types.h. Do NOT include types.h here
 * to avoid circular dependency. Use forward declarations only.
 */

#ifndef FOCUS_MANAGER_H
#define FOCUS_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

/* Forward declarations - avoid including full headers */
struct server;
struct toplevel;
struct wlr_surface;
struct wlr_scene_node;
struct wlr_scene_tree;
struct wlr_xdg_popup;

/*
 * Popup tracking data.
 * Moved here from popup.c to centralize focus-related state.
 */
struct popup_data {
    struct wlr_xdg_popup *popup;
    struct wlr_surface *surface;
    struct wlr_scene_tree *scene_tree;
    struct server *server;
    
    /* State tracking */
    int configured;
    int commit_count;
    bool mapped;
    bool has_grab;
    
    /* Listeners */
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener grab;
    
    /* Stack link (in focus_manager.popup_stack) */
    struct wl_list link;
};

/*
 * Focus transition reason - helps determine appropriate behavior.
 */
enum focus_reason {
    FOCUS_REASON_NONE,
    FOCUS_REASON_POINTER_MOTION,    /* Cursor moved to new surface */
    FOCUS_REASON_POINTER_CLICK,     /* User clicked on surface */
    FOCUS_REASON_SURFACE_MAP,       /* New surface appeared */
    FOCUS_REASON_SURFACE_UNMAP,     /* Surface disappeared */
    FOCUS_REASON_SURFACE_DESTROY,   /* Surface destroyed */
    FOCUS_REASON_POPUP_GRAB,        /* Popup requested grab */
    FOCUS_REASON_POPUP_DISMISS,     /* Popup dismissed */
    FOCUS_REASON_EXPLICIT,          /* Programmatic focus change */
};

/*
 * Focus manager state.
 * Embed this in struct server or keep as pointer.
 */
struct focus_manager {
    struct server *server;
    
    /* Current focus targets (may differ from seat state during transitions) */
    struct wlr_surface *pointer_focus;
    struct wlr_surface *keyboard_focus;
    
    /* Popup grab stack - topmost popup with grab gets keyboard focus */
    struct wl_list popup_stack;  /* struct popup_data::link */
    
    /* Deferred focus handling */
    bool pointer_focus_deferred;    /* Defer until button release */
    struct wlr_surface *deferred_pointer_target;
    double deferred_sx, deferred_sy;
    
    /* Keyboard modifier state */
    uint32_t modifier_state;
    
    /* Statistics */
    int focus_change_count;
};

/* ============== Initialization ============== */

/*
 * Initialize the focus manager.
 * Call once during server setup.
 */
void focus_manager_init(struct focus_manager *fm, struct server *server);

/*
 * Cleanup focus manager resources.
 */
void focus_manager_cleanup(struct focus_manager *fm);

/* ============== Surface Queries ============== */

/*
 * Find the surface at the current cursor position.
 * Returns NULL if no surface is under the cursor.
 * Outputs surface-local coordinates via sx/sy.
 */
struct wlr_surface *focus_surface_at_cursor(
    struct focus_manager *fm,
    double *out_sx,
    double *out_sy
);

/*
 * Find the toplevel at the current cursor position.
 * Returns NULL if cursor is not over a toplevel.
 */
struct toplevel *focus_toplevel_at_cursor(struct focus_manager *fm);

/*
 * Find the toplevel that owns a given surface.
 * Handles subsurfaces by walking up the parent chain.
 */
struct toplevel *focus_toplevel_from_surface(
    struct focus_manager *fm,
    struct wlr_surface *surface
);

/*
 * Find the toplevel from a scene node.
 * Walks up the scene tree looking for node->data.
 */
struct toplevel *focus_toplevel_from_node(
    struct focus_manager *fm,
    struct wlr_scene_node *node
);

/* ============== Pointer Focus ============== */

/*
 * Update pointer focus to a new surface.
 * Sends appropriate enter/leave events.
 * If surface is NULL, clears pointer focus.
 *
 * Respects button-held deferral: if buttons are pressed,
 * the focus change is deferred until release.
 */
void focus_pointer_set(
    struct focus_manager *fm,
    struct wlr_surface *surface,
    double sx, double sy,
    enum focus_reason reason
);

/*
 * Send pointer motion to the currently focused surface.
 */
void focus_pointer_motion(
    struct focus_manager *fm,
    double sx, double sy,
    uint32_t time_msec
);

/*
 * Clear pointer focus entirely.
 */
void focus_pointer_clear(struct focus_manager *fm);

/*
 * Recheck pointer focus based on current cursor position.
 * Called after surface geometry changes, map/unmap, etc.
 * Handles deferred focus if buttons were held.
 */
void focus_pointer_recheck(struct focus_manager *fm);

/*
 * Notify that a button was pressed.
 * This may defer focus changes until release.
 */
void focus_pointer_button_pressed(struct focus_manager *fm);

/*
 * Notify that a button was released.
 * This may trigger deferred focus changes.
 */
void focus_pointer_button_released(struct focus_manager *fm);

/* ============== Keyboard Focus ============== */

/*
 * Set keyboard focus to a surface.
 * Sends appropriate enter events with current modifier state.
 */
void focus_keyboard_set(
    struct focus_manager *fm,
    struct wlr_surface *surface,
    enum focus_reason reason
);

/*
 * Clear keyboard focus entirely.
 */
void focus_keyboard_clear(struct focus_manager *fm);

/*
 * Update modifier state and notify the focused surface.
 */
void focus_keyboard_set_modifiers(
    struct focus_manager *fm,
    uint32_t modifiers
);

/*
 * Get current modifier state.
 */
uint32_t focus_keyboard_get_modifiers(struct focus_manager *fm);

/* ============== Toplevel Focus ============== */

/*
 * Focus a toplevel window.
 * This:
 * - Deactivates the previously focused toplevel
 * - Raises the toplevel's scene node
 * - Moves toplevel to front of the list
 * - Activates the toplevel
 * - Sets keyboard focus to the toplevel's surface
 */
void fm_focus_toplevel(
    struct focus_manager *fm,
    struct toplevel *tl,
    enum focus_reason reason
);

/*
 * Get the currently focused toplevel (if any).
 */
struct toplevel *focus_get_focused_toplevel(struct focus_manager *fm);

/* ============== Popup Management ============== */

/*
 * Register a new popup with the focus manager.
 * If the popup has a grab, it will receive keyboard focus.
 */
void focus_popup_register(
    struct focus_manager *fm,
    struct popup_data *pd
);

/*
 * Unregister a popup (called on destroy).
 * Handles focus transfer to parent popup or toplevel.
 */
void focus_popup_unregister(
    struct focus_manager *fm,
    struct popup_data *pd
);

/*
 * Notify that a popup has mapped.
 * If it has a grab, transfer keyboard focus to it.
 */
void focus_popup_mapped(
    struct focus_manager *fm,
    struct popup_data *pd
);

/*
 * Notify that a popup has unmapped.
 * Handle pointer focus transfer if needed.
 */
void focus_popup_unmapped(
    struct focus_manager *fm,
    struct popup_data *pd
);

/*
 * Get the topmost popup (most recently added).
 */
struct popup_data *focus_popup_get_topmost(struct focus_manager *fm);

/*
 * Find popup by its surface.
 * Also checks subsurfaces of popups.
 */
struct popup_data *focus_popup_from_surface(
    struct focus_manager *fm,
    struct wlr_surface *surface
);

/*
 * Dismiss all popups.
 * Called when clicking outside popup stack.
 */
void focus_popup_dismiss_all(struct focus_manager *fm);

/*
 * Dismiss the topmost grabbed popup.
 * Called on Escape key.
 * Returns true if a popup was dismissed.
 */
bool focus_popup_dismiss_topmost_grabbed(struct focus_manager *fm);

/*
 * Check if any popups are active.
 */
bool focus_popup_stack_empty(struct focus_manager *fm);

/* ============== Surface Lifecycle ============== */

/*
 * Notify that a surface has mapped.
 * May trigger focus changes.
 */
void focus_on_surface_map(
    struct focus_manager *fm,
    struct wlr_surface *surface,
    bool is_toplevel
);

/*
 * Notify that a surface has unmapped.
 * Clears focus if this surface was focused.
 */
void focus_on_surface_unmap(
    struct focus_manager *fm,
    struct wlr_surface *surface
);

/*
 * Notify that a surface is being destroyed.
 * Like unmap but more urgent - must clear references.
 */
void focus_on_surface_destroy(
    struct focus_manager *fm,
    struct wlr_surface *surface
);

/* ============== Click Handling ============== */

/*
 * Handle a pointer click.
 * This is the main entry point for click-to-focus logic.
 * 
 * Behavior:
 * - If clicking outside popups, dismiss all popups
 * - If clicking on a toplevel, focus it
 * - If clicking on a popup, that's fine (no action needed)
 *
 * Returns the surface that should receive the click event,
 * which may differ from the original surface if popups were dismissed.
 */
struct wlr_surface *focus_handle_click(
    struct focus_manager *fm,
    struct wlr_surface *clicked_surface,
    double sx, double sy,
    uint32_t button
);

/* ============== Coordinate Helpers ============== */

/*
 * Convert physical coordinates to logical (for Wayland clients).
 */
static inline int focus_phys_to_logical(int phys, float scale) {
    return (int)(phys / scale + 0.5f);
}

/*
 * Convert logical coordinates to physical.
 */
static inline int focus_logical_to_phys(int logical, float scale) {
    return (int)(logical * scale + 0.5f);
}

#endif /* FOCUS_MANAGER_H */

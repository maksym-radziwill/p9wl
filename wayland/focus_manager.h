/*
 * focus_manager.h - Unified focus state machine
 *
 * This module consolidates all focus-related logic for the p9wl compositor:
 *
 *   - Pointer focus: which surface receives mouse events
 *   - Keyboard focus: which surface receives key events  
 *   - Popup management: grab stack for menus and dropdowns
 *   - Surface lifecycle: focus transitions on map/unmap/destroy
 *
 * Design Notes:
 *
 *   The focus manager maintains two independent focus targets. Pointer focus
 *   follows the cursor and determines where motion/button events are sent.
 *   Keyboard focus is typically set by clicks or popup grabs and determines
 *   where key events are sent.
 *
 *   Popup grabs form a stack. When a grabbed popup is active, it receives
 *   keyboard focus. Dismissing popups restores focus to the parent popup
 *   or the underlying toplevel.
 *
 *   Focus changes are deferred while mouse buttons are held to prevent
 *   focus from changing mid-drag.
 *
 * Include Order:
 *
 *   This header is included by types.h to define struct focus_manager and
 *   struct popup_data. Do NOT include types.h here to avoid circular
 *   dependency. Use forward declarations for struct server, struct toplevel.
 */

#ifndef FOCUS_MANAGER_H
#define FOCUS_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

/* Forward declarations (defined in types.h) */
struct server;
struct toplevel;
struct wlr_surface;
struct wlr_scene_node;
struct wlr_scene_tree;
struct wlr_xdg_popup;

/*
 * Popup tracking state.
 *
 * Each XDG popup gets a popup_data struct that tracks its lifecycle
 * and position in the grab stack. Popups with grabs (menus, dropdowns)
 * capture keyboard focus while active.
 */
struct popup_data {
    struct wlr_xdg_popup *popup;
    struct wlr_surface *surface;
    struct wlr_scene_tree *scene_tree;
    struct server *server;

    /* Lifecycle state */
    int configured;             /* Initial configure sent */
    int commit_count;           /* Number of commits received */
    bool mapped;                /* Surface has buffer and is visible */
    bool has_grab;              /* Popup requested keyboard grab */

    /* Wayland listeners */
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener grab;

    /* Link in focus_manager.popup_stack (most recent first) */
    struct wl_list link;
};

/*
 * Focus transition reasons.
 *
 * These help the focus manager decide appropriate behavior, such as
 * whether to defer focus changes or send activation events.
 */
enum focus_reason {
    FOCUS_REASON_NONE,
    FOCUS_REASON_POINTER_MOTION,    /* Cursor moved over new surface */
    FOCUS_REASON_POINTER_CLICK,     /* User clicked on surface */
    FOCUS_REASON_SURFACE_MAP,       /* Surface became visible */
    FOCUS_REASON_SURFACE_UNMAP,     /* Surface became invisible */
    FOCUS_REASON_SURFACE_DESTROY,   /* Surface is being destroyed */
    FOCUS_REASON_POPUP_GRAB,        /* Popup requested keyboard grab */
    FOCUS_REASON_POPUP_DISMISS,     /* Popup was dismissed */
    FOCUS_REASON_EXPLICIT,          /* Programmatic focus change */
};

/*
 * Focus manager state.
 *
 * Embedded in struct server. Tracks current focus targets, the popup
 * grab stack, and deferred focus state for button-held scenarios.
 */
struct focus_manager {
    struct server *server;

    /* Current focus targets (may lag seat state during transitions) */
    struct wlr_surface *pointer_focus;
    struct wlr_surface *keyboard_focus;

    /* Popup grab stack: head is most recent (topmost) popup */
    struct wl_list popup_stack;

    /* Deferred pointer focus (while buttons held) */
    bool pointer_focus_deferred;
    struct wlr_surface *deferred_pointer_target;
    double deferred_sx, deferred_sy;

    /* Cached keyboard modifier state */
    uint32_t modifier_state;

    /* Debug statistics */
    int focus_change_count;
};

/* ============== Initialization ============== */

/*
 * Initialize the focus manager. Call once during server startup.
 * Zeroes all state and initializes the popup stack list.
 */
void focus_manager_init(struct focus_manager *fm, struct server *server);

/*
 * Clean up focus manager resources. Logs statistics.
 */
void focus_manager_cleanup(struct focus_manager *fm);

/* ============== Surface Queries ============== */

/*
 * Find the surface under the current cursor position.
 *
 * Performs a scene graph hit test at the cursor location. If a mapped
 * surface is found, returns it and outputs surface-local coordinates.
 * Falls back to the first mapped toplevel if no surface is directly
 * under the cursor.
 *
 * Returns NULL if no surface is available.
 */
struct wlr_surface *focus_surface_at_cursor(
    struct focus_manager *fm,
    double *out_sx,
    double *out_sy
);

/*
 * Find the toplevel that owns a surface.
 *
 * If the surface is a subsurface, walks up the parent chain to find
 * the owning toplevel. Returns NULL if not found.
 */
struct toplevel *focus_toplevel_from_surface(
    struct focus_manager *fm,
    struct wlr_surface *surface
);

/*
 * Find the toplevel under the current cursor position.
 *
 * Convenience function combining focus_surface_at_cursor and
 * focus_toplevel_from_surface.
 */
struct toplevel *focus_toplevel_at_cursor(struct focus_manager *fm);

/* ============== Pointer Focus ============== */

/*
 * Set pointer focus to a surface.
 *
 * Sends wl_pointer.enter to the new surface and wl_pointer.leave to
 * the previous surface (if any). The sx/sy coordinates are surface-local.
 *
 * If mouse buttons are currently held and the reason is not EXPLICIT
 * or SURFACE_DESTROY, the focus change is deferred until buttons are
 * released. This prevents focus changes during drag operations.
 *
 * Pass NULL to clear pointer focus entirely.
 */
void focus_pointer_set(
    struct focus_manager *fm,
    struct wlr_surface *surface,
    double sx, double sy,
    enum focus_reason reason
);

/*
 * Send pointer motion to the currently focused surface.
 *
 * Call this after updating the cursor position. The sx/sy coordinates
 * are surface-local. Does nothing if no surface has pointer focus.
 */
void focus_pointer_motion(
    struct focus_manager *fm,
    double sx, double sy,
    uint32_t time_msec
);

/*
 * Recheck pointer focus after surface geometry changes.
 *
 * Call this after surfaces map, unmap, or change size. Performs a new
 * hit test and updates focus if the surface under the cursor changed.
 *
 * Also processes any deferred focus changes if buttons are no longer held.
 */
void focus_pointer_recheck(struct focus_manager *fm);

/*
 * Notify that a mouse button was pressed.
 *
 * Currently a no-op; the seat tracks button count internally.
 * Kept for API symmetry with focus_pointer_button_released.
 */
void focus_pointer_button_pressed(struct focus_manager *fm);

/*
 * Notify that a mouse button was released.
 *
 * If all buttons are now released and focus was deferred, triggers
 * focus_pointer_recheck to process the deferred focus change.
 */
void focus_pointer_button_released(struct focus_manager *fm);

/* ============== Keyboard Focus ============== */

/*
 * Set keyboard focus to a surface.
 *
 * Sends wl_keyboard.enter with the current keymap and modifier state.
 * Pass NULL to clear keyboard focus.
 */
void focus_keyboard_set(
    struct focus_manager *fm,
    struct wlr_surface *surface,
    enum focus_reason reason
);

/*
 * Update modifier state and notify the focused surface.
 *
 * Called when modifier keys (Shift, Ctrl, Alt, etc.) change state.
 * The modifiers parameter uses WLR_MODIFIER_* flags.
 */
void focus_keyboard_set_modifiers(
    struct focus_manager *fm,
    uint32_t modifiers
);

/*
 * Get the current modifier state.
 */
uint32_t focus_keyboard_get_modifiers(struct focus_manager *fm);

/* ============== Toplevel Focus ============== */

/*
 * Focus a toplevel window.
 *
 * This is the main entry point for focusing application windows:
 *   1. Deactivates the previously focused toplevel
 *   2. Raises the toplevel's scene node to the top
 *   3. Moves the toplevel to the front of the server's list
 *   4. Activates the toplevel (sends xdg_toplevel.configure)
 *   5. Sets keyboard focus to the toplevel's surface
 *
 * Does nothing if the toplevel is already focused or invalid.
 */
void focus_toplevel(
    struct focus_manager *fm,
    struct toplevel *tl,
    enum focus_reason reason
);

/*
 * Get the currently focused toplevel.
 *
 * Returns the toplevel that owns the current keyboard focus surface,
 * or NULL if keyboard focus is on a popup or cleared.
 */
struct toplevel *focus_get_focused_toplevel(struct focus_manager *fm);

/* ============== Popup Management ============== */

/*
 * Register a popup with the focus manager.
 *
 * Adds the popup to the grab stack. Call this when a popup is created,
 * before it maps. The popup will be at the top of the stack.
 */
void focus_popup_register(
    struct focus_manager *fm,
    struct popup_data *pd
);

/*
 * Unregister a popup from the focus manager.
 *
 * Removes the popup from the grab stack and restores focus to the
 * next popup in the stack or the underlying toplevel. Call this
 * when the popup is destroyed.
 */
void focus_popup_unregister(
    struct focus_manager *fm,
    struct popup_data *pd
);

/*
 * Notify that a popup has mapped (become visible).
 *
 * If the popup has a grab, transfers keyboard focus to it.
 * Also rechecks pointer focus in case the popup appeared under cursor.
 */
void focus_popup_mapped(
    struct focus_manager *fm,
    struct popup_data *pd
);

/*
 * Notify that a popup has unmapped (become invisible).
 *
 * If the popup had pointer focus, transfers it to the next available
 * surface.
 */
void focus_popup_unmapped(
    struct focus_manager *fm,
    struct popup_data *pd
);

/*
 * Get the topmost popup in the grab stack.
 *
 * Returns NULL if no popups are active.
 */
struct popup_data *focus_popup_get_topmost(struct focus_manager *fm);

/*
 * Find the popup that owns a surface.
 *
 * Checks direct ownership and subsurface chains.
 * Returns NULL if the surface is not part of any popup.
 */
struct popup_data *focus_popup_from_surface(
    struct focus_manager *fm,
    struct wlr_surface *surface
);

/*
 * Dismiss all active popups.
 *
 * Destroys every popup in the grab stack. Called when the user clicks
 * outside the popup hierarchy.
 */
void focus_popup_dismiss_all(struct focus_manager *fm);

/*
 * Dismiss the topmost grabbed popup.
 *
 * Called when the user presses Escape. Only dismisses popups that
 * have an active grab (menus, dropdowns).
 *
 * Returns true if a popup was dismissed, false if no grabbed popup exists.
 */
bool focus_popup_dismiss_topmost_grabbed(struct focus_manager *fm);

/*
 * Check if the popup stack is empty.
 */
bool focus_popup_stack_empty(struct focus_manager *fm);

/* ============== Surface Lifecycle ============== */

/*
 * Notify that a surface has mapped (become visible).
 *
 * For toplevels, focuses the new window. For all surfaces, updates
 * pointer focus if the surface appeared under the cursor.
 */
void focus_on_surface_map(
    struct focus_manager *fm,
    struct wlr_surface *surface,
    bool is_toplevel
);

/*
 * Notify that a surface has unmapped (become invisible).
 *
 * If the surface had pointer or keyboard focus, transfers focus to
 * the next available surface (popup or toplevel).
 */
void focus_on_surface_unmap(
    struct focus_manager *fm,
    struct wlr_surface *surface
);

/*
 * Notify that a surface is being destroyed.
 *
 * Like focus_on_surface_unmap, but also clears any deferred focus
 * references to prevent use-after-free.
 */
void focus_on_surface_destroy(
    struct focus_manager *fm,
    struct wlr_surface *surface
);

/* ============== Click Handling ============== */

/*
 * Handle a pointer click for focus management.
 *
 * This is the main entry point for click-to-focus logic:
 *
 *   - Click on popup: keep current focus (return the popup surface)
 *   - Click outside popups: dismiss all popups, focus clicked surface
 *   - Click on toplevel: focus that toplevel
 *
 * Returns the surface that should receive the button event, which may
 * differ from the clicked surface if popups were dismissed.
 */
struct wlr_surface *focus_handle_click(
    struct focus_manager *fm,
    struct wlr_surface *clicked_surface,
    double sx, double sy,
    uint32_t button
);

/* ============== Coordinate Conversion ============== */

/*
 * Convert physical (pixel) coordinates to logical (Wayland) coordinates.
 *
 * Wayland clients work in logical coordinates; the compositor works in
 * physical pixels. This conversion is needed for HiDPI scaling.
 */
static inline int focus_phys_to_logical(int phys, float scale) {
    return (int)(phys / scale + 0.5f);
}

/*
 * Convert logical (Wayland) coordinates to physical (pixel) coordinates.
 */
static inline int focus_logical_to_phys(int logical, float scale) {
    return (int)(logical * scale + 0.5f);
}

#endif /* FOCUS_MANAGER_H */

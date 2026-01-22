/*
 * focus.c - Focus management (refactored)
 *
 * This file now serves as a thin wrapper around focus_manager.
 * Legacy function signatures are preserved for compatibility.
 *
 * The actual focus logic has been consolidated in focus_manager.c
 */

#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "focus.h"
#include "../types.h"  /* Includes focus_manager.h */

/*
 * Legacy API wrappers - these delegate to focus_manager.
 * Kept for backward compatibility with existing code.
 */

struct toplevel *toplevel_from_surface(struct server *s, struct wlr_surface *surface) {
    return focus_toplevel_from_surface(&s->focus, surface);
}

struct toplevel *toplevel_from_node(struct wlr_scene_node *node) {
    /* This doesn't need server context, use the standalone version */
    while (node) {
        if (node->data) {
            return (struct toplevel *)node->data;
        }
        if (node->parent) {
            node = &node->parent->node;
        } else {
            break;
        }
    }
    return NULL;
}

struct wlr_surface *surface_at_cursor(struct server *s, double *out_sx, double *out_sy) {
    return focus_surface_at_cursor(&s->focus, out_sx, out_sy);
}

struct toplevel *toplevel_at_cursor(struct server *s) {
    return focus_toplevel_at_cursor(&s->focus);
}

void focus_toplevel(struct server *s, struct toplevel *tl) {
    fm_focus_toplevel(&s->focus, tl, FOCUS_REASON_EXPLICIT);
}

void recheck_pointer_focus(struct server *s) {
    focus_pointer_recheck(&s->focus);
}

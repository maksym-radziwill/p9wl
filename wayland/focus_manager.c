/*
 * focus_manager.c - Pointer focus, keyboard focus, popup grab stack.
 *
 * Fixed from original:
 * - focus_pointer_recheck: always does fresh hit test on button release
 *   (old code skipped hit test when deferred_pointer_target was NULL)
 * - focus_popup_unregister: uses focus_pointer_set instead of bypassing
 *   it with direct wlr_seat calls (was skipping fm->pointer_focus update)
 * - focus_toplevel_from_surface: walks to root surface once, then scans
 *   (was O(n*m) walking subsurface chain per toplevel)
 * - focus_popup_from_surface: reuses root_surface instead of duplicating
 *   the subsurface walk
 * - focus_on_surface_unmap: calls fallback_surface once instead of twice
 * - Consistent use of tl->surface (was mixing with tl->xdg->base->surface)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include "types.h"

#define SEAT(fm)         ((fm)->server->seat)
#define CURSOR(fm)       ((fm)->server->cursor)
#define BUTTONS_HELD(fm) (SEAT(fm)->pointer_state.button_count > 0)

void focus_manager_init(struct focus_manager *fm, struct server *server) {
    memset(fm, 0, sizeof(*fm));
    fm->server = server;
    wl_list_init(&fm->popup_stack);
}

void focus_manager_cleanup(struct focus_manager *fm) {
    wlr_log(WLR_INFO, "Focus: %d changes", fm->focus_change_count);
}

/* Walk subsurface parents to find root surface. */
static struct wlr_surface *root_surface(struct wlr_surface *surface) {
    while (surface) {
        struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(surface);
        if (!sub) break;
        surface = sub->parent;
    }
    return surface;
}

/* Find any mapped surface to give focus to, skipping `skip`. */
static struct wlr_surface *fallback_surface(struct focus_manager *fm,
                                             struct wlr_surface *skip) {
    struct popup_data *pd;
    wl_list_for_each(pd, &fm->popup_stack, link) {
        if (pd->mapped && pd->surface != skip)
            return pd->surface;
    }
    struct toplevel *tl;
    wl_list_for_each(tl, &fm->server->toplevels, link) {
        if (tl->mapped && tl->surface != skip)
            return tl->surface;
    }
    return NULL;
}

/* ============== Surface Queries ============== */

struct wlr_surface *focus_surface_at_cursor(struct focus_manager *fm,
                                            double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(
        &fm->server->scene->tree.node, CURSOR(fm)->x, CURSOR(fm)->y, sx, sy);

    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
        if (ss && ss->surface && ss->surface->mapped)
            return ss->surface;
    }

    /* Nothing under cursor — fall back to first mapped toplevel */
    struct toplevel *tl;
    wl_list_for_each(tl, &fm->server->toplevels, link) {
        if (tl->mapped) {
            *sx = CURSOR(fm)->x;
            *sy = CURSOR(fm)->y;
            return tl->surface;
        }
    }
    return NULL;
}

struct toplevel *focus_toplevel_from_surface(struct focus_manager *fm,
                                             struct wlr_surface *surface) {
    if (!surface) return NULL;
    struct wlr_surface *root = root_surface(surface);
    struct toplevel *tl;
    wl_list_for_each(tl, &fm->server->toplevels, link) {
        if (tl->surface == root) return tl;
    }
    return NULL;
}

struct toplevel *focus_toplevel_at_cursor(struct focus_manager *fm) {
    double sx, sy;
    struct wlr_surface *s = focus_surface_at_cursor(fm, &sx, &sy);
    return s ? focus_toplevel_from_surface(fm, s) : NULL;
}

/* ============== Pointer Focus ============== */

void focus_pointer_set(struct focus_manager *fm, struct wlr_surface *surface,
                       double sx, double sy, enum focus_reason reason) {
    if (surface == SEAT(fm)->pointer_state.focused_surface)
        return;

    /* Defer if dragging (except forced cases) */
    if (BUTTONS_HELD(fm) && reason != FOCUS_REASON_EXPLICIT &&
        reason != FOCUS_REASON_SURFACE_DESTROY) {
        fm->pointer_focus_deferred = true;
        fm->deferred_pointer_target = surface;
        fm->deferred_sx = sx;
        fm->deferred_sy = sy;
        return;
    }

    fm->focus_change_count++;
    fm->pointer_focus = surface;

    if (surface)
        wlr_seat_pointer_notify_enter(SEAT(fm), surface, sx, sy);
    else
        wlr_seat_pointer_notify_clear_focus(SEAT(fm));
    wlr_seat_pointer_notify_frame(SEAT(fm));
}

void focus_pointer_motion(struct focus_manager *fm, double sx, double sy,
                          uint32_t time_msec) {
    wlr_seat_pointer_notify_motion(SEAT(fm), time_msec, sx, sy);
}

/*
 * Re-evaluate pointer focus after geometry changes.
 * If buttons are held, do nothing — focus_pointer_set will defer.
 * Otherwise, always do a fresh hit test. This fixes the old bug where
 * a stale/NULL deferred_pointer_target caused the hit test to be skipped.
 */
void focus_pointer_recheck(struct focus_manager *fm) {
    if (BUTTONS_HELD(fm))
        return;

    fm->pointer_focus_deferred = false;
    fm->deferred_pointer_target = NULL;

    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    if (surface != SEAT(fm)->pointer_state.focused_surface)
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
}

void focus_pointer_button_pressed(struct focus_manager *fm) {
    (void)fm;
}

void focus_pointer_button_released(struct focus_manager *fm) {
    if (fm->pointer_focus_deferred && !BUTTONS_HELD(fm))
        focus_pointer_recheck(fm);
}

/* ============== Keyboard Focus ============== */

void focus_keyboard_set(struct focus_manager *fm, struct wlr_surface *surface,
                        enum focus_reason reason) {
    (void)reason;
    if (surface == SEAT(fm)->keyboard_state.focused_surface)
        return;

    fm->keyboard_focus = surface;
    fm->focus_change_count++;

    if (surface) {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(SEAT(fm));
        if (kb)
            wlr_seat_keyboard_notify_enter(SEAT(fm), surface,
                kb->keycodes, kb->num_keycodes, &kb->modifiers);
    } else {
        wlr_seat_keyboard_notify_clear_focus(SEAT(fm));
    }
}

void focus_keyboard_set_modifiers(struct focus_manager *fm, uint32_t modifiers) {
    fm->modifier_state = modifiers;
    struct wlr_keyboard_modifiers mods = { .depressed = modifiers };
    wlr_seat_keyboard_notify_modifiers(SEAT(fm), &mods);
}

uint32_t focus_keyboard_get_modifiers(struct focus_manager *fm) {
    return fm->modifier_state;
}

/* ============== Toplevel Focus ============== */

void focus_toplevel(struct focus_manager *fm, struct toplevel *tl,
                    enum focus_reason reason) {
    if (!tl || !tl->xdg) return;
    if (tl->surface == SEAT(fm)->keyboard_state.focused_surface) return;

    /* Deactivate previous */
    struct toplevel *prev = focus_get_focused_toplevel(fm);
    if (prev && prev->xdg)
        wlr_xdg_toplevel_set_activated(prev->xdg, false);

    /* Raise, reorder, activate */
    wlr_scene_node_raise_to_top(&tl->scene_tree->node);
    wl_list_remove(&tl->link);
    wl_list_insert(&fm->server->toplevels, &tl->link);
    wlr_xdg_toplevel_set_activated(tl->xdg, true);
    focus_keyboard_set(fm, tl->surface, reason);
}

struct toplevel *focus_get_focused_toplevel(struct focus_manager *fm) {
    struct wlr_surface *s = SEAT(fm)->keyboard_state.focused_surface;
    return s ? focus_toplevel_from_surface(fm, s) : NULL;
}

/* ============== Popup Management ============== */

void focus_popup_register(struct focus_manager *fm, struct popup_data *pd) {
    wl_list_insert(&fm->popup_stack, &pd->link);
}

void focus_popup_mapped(struct focus_manager *fm, struct popup_data *pd) {
    if (pd->has_grab)
        focus_keyboard_set(fm, pd->surface, FOCUS_REASON_POPUP_GRAB);
    focus_pointer_recheck(fm);
}

void focus_popup_unmapped(struct focus_manager *fm, struct popup_data *pd) {
    if (SEAT(fm)->pointer_state.focused_surface != pd->surface)
        return;

    struct wlr_surface *target = fallback_surface(fm, pd->surface);
    if (target)
        focus_pointer_set(fm, target, CURSOR(fm)->x, CURSOR(fm)->y,
                          FOCUS_REASON_SURFACE_UNMAP);
    else
        focus_pointer_set(fm, NULL, 0, 0, FOCUS_REASON_EXPLICIT);
}

/*
 * Popup destroyed — restore focus.
 * Uses focus_pointer_set instead of direct wlr_seat calls so that
 * fm->pointer_focus stays in sync and deferral logic is respected.
 */
void focus_popup_unregister(struct focus_manager *fm, struct popup_data *pd) {
    bool had_grab = pd->has_grab;
    struct wlr_surface *pd_surface = pd->surface;

    wl_list_remove(&pd->link);
    wl_list_init(&pd->link);

    /* Clear pointer focus for destroyed popup */
    focus_pointer_set(fm, NULL, 0, 0, FOCUS_REASON_EXPLICIT);

    /* Find new target */
    struct wlr_surface *target = fallback_surface(fm, pd_surface);
    if (target) {
        focus_pointer_set(fm, target, CURSOR(fm)->x, CURSOR(fm)->y,
                          FOCUS_REASON_POPUP_DISMISS);
        if (had_grab)
            focus_keyboard_set(fm, target, FOCUS_REASON_POPUP_DISMISS);
    }

    /* Re-activate toplevel if popup stack empty */
    if (wl_list_empty(&fm->popup_stack)) {
        struct toplevel *tl;
        wl_list_for_each(tl, &fm->server->toplevels, link) {
            if (tl->mapped && tl->xdg) {
                wlr_xdg_toplevel_set_activated(tl->xdg, true);
                if (!had_grab)
                    focus_keyboard_set(fm, tl->surface, FOCUS_REASON_POPUP_DISMISS);
                break;
            }
        }
    }
}

struct popup_data *focus_popup_get_topmost(struct focus_manager *fm) {
    if (wl_list_empty(&fm->popup_stack)) return NULL;
    struct popup_data *pd;
    return wl_container_of(fm->popup_stack.next, pd, link);
}

struct popup_data *focus_popup_from_surface(struct focus_manager *fm,
                                             struct wlr_surface *surface) {
    struct wlr_surface *root = root_surface(surface);
    struct popup_data *pd;
    wl_list_for_each(pd, &fm->popup_stack, link) {
        if (pd->surface == surface || pd->surface == root)
            return pd;
    }
    return NULL;
}

void focus_popup_dismiss_all(struct focus_manager *fm) {
    struct popup_data *pd, *tmp;
    wl_list_for_each_safe(pd, tmp, &fm->popup_stack, link) {
        wlr_xdg_popup_destroy(pd->popup);
    }
}

bool focus_popup_dismiss_topmost_grabbed(struct focus_manager *fm) {
    if (wl_list_empty(&fm->popup_stack)) return false;
    struct popup_data *pd;
    pd = wl_container_of(fm->popup_stack.next, pd, link);
    if (!pd->has_grab) return false;
    wlr_xdg_popup_destroy(pd->popup);
    return true;
}

bool focus_popup_stack_empty(struct focus_manager *fm) {
    return wl_list_empty(&fm->popup_stack);
}

/* ============== Surface Lifecycle ============== */

void focus_on_surface_map(struct focus_manager *fm, struct wlr_surface *surface,
                          bool is_toplevel) {
    if (is_toplevel) {
        struct toplevel *tl = focus_toplevel_from_surface(fm, surface);
        if (tl) focus_toplevel(fm, tl, FOCUS_REASON_SURFACE_MAP);
    }

    double sx, sy;
    struct wlr_surface *under = focus_surface_at_cursor(fm, &sx, &sy);
    if (under == surface)
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_SURFACE_MAP);
}

void focus_on_surface_unmap(struct focus_manager *fm, struct wlr_surface *surface) {
    struct wlr_surface *ptr_focused = SEAT(fm)->pointer_state.focused_surface;
    struct wlr_surface *kbd_focused = SEAT(fm)->keyboard_state.focused_surface;

    if (ptr_focused != surface && kbd_focused != surface)
        return;

    struct wlr_surface *target = fallback_surface(fm, surface);

    if (ptr_focused == surface) {
        if (target)
            focus_pointer_set(fm, target, CURSOR(fm)->x, CURSOR(fm)->y,
                              FOCUS_REASON_SURFACE_UNMAP);
        else
            focus_pointer_set(fm, NULL, 0, 0, FOCUS_REASON_EXPLICIT);
    }

    if (kbd_focused == surface)
        focus_keyboard_set(fm, target, FOCUS_REASON_SURFACE_UNMAP);
}

void focus_on_surface_destroy(struct focus_manager *fm, struct wlr_surface *surface) {
    focus_on_surface_unmap(fm, surface);

    if (fm->deferred_pointer_target == surface) {
        fm->deferred_pointer_target = NULL;
        fm->pointer_focus_deferred = false;
    }
}

/* ============== Click Handling ============== */

struct wlr_surface *focus_handle_click(struct focus_manager *fm,
                                        struct wlr_surface *clicked,
                                        double sx, double sy,
                                        uint32_t button) {
    (void)button;

    /* Click on popup — keep it focused */
    if (focus_popup_from_surface(fm, clicked))
        return clicked;

    /* Click outside popup stack — dismiss all */
    if (!wl_list_empty(&fm->popup_stack)) {
        focus_popup_dismiss_all(fm);
        struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
        if (surface)
            focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_CLICK);
        return surface;
    }

    /* Click on toplevel */
    struct toplevel *tl = focus_toplevel_from_surface(fm, clicked);
    if (tl) focus_toplevel(fm, tl, FOCUS_REASON_POINTER_CLICK);

    return clicked;
}

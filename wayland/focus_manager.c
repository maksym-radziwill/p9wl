/*
 * focus_manager.c - Pointer focus, keyboard focus, popup grab stack.
 *
 * The wlr_seat owns the real focus state. This module adds:
 * - Pointer focus deferral while buttons are held (prevent mid-drag refocus)
 * - Popup grab stack with keyboard focus transfer
 * - Fallback focus on surface unmap/destroy
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
#define PTR_FOCUSED(fm)  (SEAT(fm)->pointer_state.focused_surface)
#define KBD_FOCUSED(fm)  (SEAT(fm)->keyboard_state.focused_surface)

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

/* First mapped surface that isn't `skip`. Checks popups then toplevels. */
static struct wlr_surface *fallback_surface(struct focus_manager *fm,
                                             struct wlr_surface *skip) {
    struct popup_data *pd;
    wl_list_for_each(pd, &fm->popup_stack, link)
        if (pd->mapped && pd->surface != skip)
            return pd->surface;

    struct toplevel *tl;
    wl_list_for_each(tl, &fm->server->toplevels, link)
        if (tl->mapped && tl->surface != skip)
            return tl->surface;

    return NULL;
}

/* ============== Queries ============== */

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
    wl_list_for_each(tl, &fm->server->toplevels, link)
        if (tl->surface == root) return tl;
    return NULL;
}

struct toplevel *focus_toplevel_at_cursor(struct focus_manager *fm) {
    double sx, sy;
    struct wlr_surface *s = focus_surface_at_cursor(fm, &sx, &sy);
    return s ? focus_toplevel_from_surface(fm, s) : NULL;
}

/* ============== Pointer Focus ============== */

void focus_pointer_set(struct focus_manager *fm, struct wlr_surface *surface,
                       double sx, double sy, bool force) {
    if (surface == PTR_FOCUSED(fm))
        return;

    if (BUTTONS_HELD(fm) && !force) {
        fm->pointer_deferred = true;
        return;
    }

    fm->focus_change_count++;
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

void focus_pointer_recheck(struct focus_manager *fm) {
    if (BUTTONS_HELD(fm))
        return;

    fm->pointer_deferred = false;

    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    if (surface != PTR_FOCUSED(fm))
        focus_pointer_set(fm, surface, sx, sy, false);
}

void focus_pointer_button_released(struct focus_manager *fm) {
    if (fm->pointer_deferred && !BUTTONS_HELD(fm))
        focus_pointer_recheck(fm);
}

/* ============== Keyboard Focus ============== */

void focus_keyboard_set(struct focus_manager *fm, struct wlr_surface *surface) {
    if (surface == KBD_FOCUSED(fm))
        return;

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

void focus_toplevel(struct focus_manager *fm, struct toplevel *tl) {
    if (!tl || !tl->xdg) return;
    if (tl->surface == KBD_FOCUSED(fm)) return;

    struct toplevel *prev = focus_get_focused_toplevel(fm);
    if (prev && prev->xdg)
        wlr_xdg_toplevel_set_activated(prev->xdg, false);

    wlr_scene_node_raise_to_top(&tl->scene_tree->node);
    wl_list_remove(&tl->link);
    wl_list_insert(&fm->server->toplevels, &tl->link);
    wlr_xdg_toplevel_set_activated(tl->xdg, true);
    focus_keyboard_set(fm, tl->surface);
}

struct toplevel *focus_get_focused_toplevel(struct focus_manager *fm) {
    struct wlr_surface *s = KBD_FOCUSED(fm);
    return s ? focus_toplevel_from_surface(fm, s) : NULL;
}

/* ============== Popup Stack ============== */

void focus_popup_register(struct focus_manager *fm, struct popup_data *pd) {
    wl_list_insert(&fm->popup_stack, &pd->link);
}

void focus_popup_mapped(struct focus_manager *fm, struct popup_data *pd) {
    if (pd->has_grab)
        focus_keyboard_set(fm, pd->surface);
    focus_pointer_recheck(fm);
}

void focus_popup_unmapped(struct focus_manager *fm, struct popup_data *pd) {
    if (PTR_FOCUSED(fm) == pd->surface) {
        struct wlr_surface *target = fallback_surface(fm, pd->surface);
        if (target)
            focus_pointer_set(fm, target, CURSOR(fm)->x, CURSOR(fm)->y, false);
        else
            focus_pointer_set(fm, NULL, 0, 0, true);
    }
}

void focus_popup_unregister(struct focus_manager *fm, struct popup_data *pd) {
    bool had_grab = pd->has_grab;
    struct wlr_surface *pd_surface = pd->surface;

    wl_list_remove(&pd->link);
    wl_list_init(&pd->link);

    /* Find something else to focus */
    struct wlr_surface *target = fallback_surface(fm, pd_surface);

    /* Pointer: clear old, set new */
    if (PTR_FOCUSED(fm) == pd_surface || !target)
        focus_pointer_set(fm, NULL, 0, 0, true);
    if (target)
        focus_pointer_set(fm, target, CURSOR(fm)->x, CURSOR(fm)->y, false);

    /* Keyboard: restore if popup had grab */
    if (had_grab && target)
        focus_keyboard_set(fm, target);

    /* Re-activate toplevel if popup stack is now empty */
    if (wl_list_empty(&fm->popup_stack)) {
        struct toplevel *tl;
        wl_list_for_each(tl, &fm->server->toplevels, link) {
            if (tl->mapped && tl->xdg) {
                wlr_xdg_toplevel_set_activated(tl->xdg, true);
                if (!had_grab)
                    focus_keyboard_set(fm, tl->surface);
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
    wl_list_for_each(pd, &fm->popup_stack, link)
        if (pd->surface == surface || pd->surface == root)
            return pd;
    return NULL;
}

void focus_popup_dismiss_all(struct focus_manager *fm) {
    struct popup_data *pd, *tmp;
    wl_list_for_each_safe(pd, tmp, &fm->popup_stack, link)
        wlr_xdg_popup_destroy(pd->popup);
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
        if (tl) focus_toplevel(fm, tl);
    }
    focus_pointer_recheck(fm);
}

void focus_on_surface_unmap(struct focus_manager *fm, struct wlr_surface *surface) {
    bool had_ptr = (PTR_FOCUSED(fm) == surface);
    bool had_kbd = (KBD_FOCUSED(fm) == surface);

    if (!had_ptr && !had_kbd)
        return;

    struct wlr_surface *target = fallback_surface(fm, surface);

    if (had_ptr) {
        if (target)
            focus_pointer_set(fm, target, CURSOR(fm)->x, CURSOR(fm)->y, true);
        else
            focus_pointer_set(fm, NULL, 0, 0, true);
    }

    if (had_kbd)
        focus_keyboard_set(fm, target);
}

void focus_on_surface_destroy(struct focus_manager *fm, struct wlr_surface *surface) {
    focus_on_surface_unmap(fm, surface);
}

/* ============== Click Handling ============== */

struct wlr_surface *focus_handle_click(struct focus_manager *fm,
                                        struct wlr_surface *clicked,
                                        double sx, double sy,
                                        uint32_t button) {
    (void)button;

    if (focus_popup_from_surface(fm, clicked))
        return clicked;

    if (!wl_list_empty(&fm->popup_stack)) {
        focus_popup_dismiss_all(fm);
        struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
        if (surface)
            focus_pointer_set(fm, surface, sx, sy, false);
        return surface;
    }

    struct toplevel *tl = focus_toplevel_from_surface(fm, clicked);
    if (tl) focus_toplevel(fm, tl);

    return clicked;
}

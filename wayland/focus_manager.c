/*
 * focus_manager.c - Unified focus state machine (streamlined)
 *
 * Removed:
 * - Trivial wrapper functions (fm_seat, fm_cursor, *_clear, button_pressed)
 * - Separate register/unregister vs mapped/unmapped (combined)
 * - Redundant validity checks where invariants hold
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

/* Macros instead of trivial helper functions */
#define SEAT(fm)         ((fm)->server->seat)
#define CURSOR(fm)       ((fm)->server->cursor)
#define BUTTONS_HELD(fm) (SEAT(fm)->pointer_state.button_count > 0)

/* ============== Initialization ============== */

void focus_manager_init(struct focus_manager *fm, struct server *server) {
    memset(fm, 0, sizeof(*fm));
    fm->server = server;
    wl_list_init(&fm->popup_stack);
}

void focus_manager_cleanup(struct focus_manager *fm) {
    wlr_log(WLR_INFO, "Focus manager: %d focus changes", fm->focus_change_count);
}

/* ============== Internal Helpers ============== */

static void send_keyboard_enter(struct focus_manager *fm, struct wlr_surface *surface) {
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(SEAT(fm));
    if (kb && surface) {
        wlr_seat_keyboard_notify_enter(SEAT(fm), surface,
            kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
}

static struct wlr_surface *find_fallback_focus(struct focus_manager *fm,
                                                struct wlr_surface *excluding) {
    /* Check popup stack first */
    struct popup_data *pd;
    wl_list_for_each(pd, &fm->popup_stack, link) {
        if (pd->surface != excluding && pd->mapped && pd->surface->mapped)
            return pd->surface;
    }
    
    /* Fall back to first mapped toplevel */
    struct toplevel *tl;
    wl_list_for_each(tl, &fm->server->toplevels, link) {
        if (tl->surface != excluding && tl->mapped && tl->xdg &&
            tl->xdg->base && tl->xdg->base->surface && tl->xdg->base->surface->mapped)
            return tl->xdg->base->surface;
    }
    
    return NULL;
}

/* ============== Surface Queries ============== */

struct wlr_surface *focus_surface_at_cursor(struct focus_manager *fm,
                                            double *out_sx, double *out_sy) {
    struct server *s = fm->server;
    double sx, sy;
    
    struct wlr_scene_node *node = wlr_scene_node_at(
        &s->scene->tree.node, CURSOR(fm)->x, CURSOR(fm)->y, &sx, &sy);
    
    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
        if (ss && ss->surface && ss->surface->mapped) {
            *out_sx = sx;
            *out_sy = sy;
            return ss->surface;
        }
    }
    
    /* Fallback to first mapped toplevel */
    if (!wl_list_empty(&s->toplevels)) {
        struct toplevel *tl = wl_container_of(s->toplevels.next, tl, link);
        if (tl->mapped && tl->xdg && tl->xdg->base && tl->xdg->base->surface) {
            *out_sx = CURSOR(fm)->x;
            *out_sy = CURSOR(fm)->y;
            return tl->xdg->base->surface;
        }
    }
    
    return NULL;
}

struct toplevel *focus_toplevel_from_surface(struct focus_manager *fm,
                                              struct wlr_surface *surface) {
    if (!surface) return NULL;
    
    struct toplevel *tl;
    wl_list_for_each(tl, &fm->server->toplevels, link) {
        if (tl->surface == surface) return tl;
        
        /* Check subsurface chain */
        struct wlr_surface *check = surface;
        while (check) {
            struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(check);
            if (!sub) break;
            check = sub->parent;
            if (tl->surface == check) return tl;
        }
    }
    
    return NULL;
}

struct toplevel *focus_toplevel_at_cursor(struct focus_manager *fm) {
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    return surface ? focus_toplevel_from_surface(fm, surface) : NULL;
}

/* ============== Pointer Focus ============== */

void focus_pointer_set(struct focus_manager *fm, struct wlr_surface *surface,
                       double sx, double sy, enum focus_reason reason) {
    struct wlr_surface *current = SEAT(fm)->pointer_state.focused_surface;
    if (surface == current) return;
    
    /* Defer focus change if buttons held (except explicit/destroy) */
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
    
    if (surface) {
        wlr_seat_pointer_notify_enter(SEAT(fm), surface, sx, sy);
    } else {
        wlr_seat_pointer_notify_clear_focus(SEAT(fm));
    }
    wlr_seat_pointer_notify_frame(SEAT(fm));
}

void focus_pointer_motion(struct focus_manager *fm, double sx, double sy,
                          uint32_t time_msec) {
    wlr_seat_pointer_notify_motion(SEAT(fm), time_msec, sx, sy);
}

void focus_pointer_recheck(struct focus_manager *fm) {
    /* Handle deferred focus when buttons released */
    if (fm->pointer_focus_deferred && !BUTTONS_HELD(fm)) {
        fm->pointer_focus_deferred = false;
        if (fm->deferred_pointer_target) {
            focus_pointer_set(fm, fm->deferred_pointer_target,
                              fm->deferred_sx, fm->deferred_sy,
                              FOCUS_REASON_POINTER_MOTION);
            fm->deferred_pointer_target = NULL;
        }
        return;
    }
    
    if (BUTTONS_HELD(fm)) {
        fm->pointer_focus_deferred = true;
        return;
    }
    
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    if (surface != SEAT(fm)->pointer_state.focused_surface) {
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
    }
}

/* Called when buttons released - check for deferred focus */
void focus_pointer_button_released(struct focus_manager *fm) {
    if (fm->pointer_focus_deferred && !BUTTONS_HELD(fm))
        focus_pointer_recheck(fm);
}

/* Stub - seat tracks button count internally */
void focus_pointer_button_pressed(struct focus_manager *fm) {
    (void)fm;
}

/* ============== Keyboard Focus ============== */

void focus_keyboard_set(struct focus_manager *fm, struct wlr_surface *surface,
                        enum focus_reason reason) {
    if (surface == SEAT(fm)->keyboard_state.focused_surface) return;
    
    fm->keyboard_focus = surface;
    fm->focus_change_count++;
    
    if (surface) {
        wlr_log(WLR_DEBUG, "Focus: keyboard -> %p reason=%d", (void*)surface, reason);
        send_keyboard_enter(fm, surface);
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
    if (!tl || !tl->xdg || !tl->xdg->base || !tl->xdg->base->initialized) return;
    
    struct wlr_surface *surface = tl->xdg->base->surface;
    if (surface == SEAT(fm)->keyboard_state.focused_surface) return;
    
    wlr_log(WLR_INFO, "Focus: toplevel %p reason=%d", (void*)tl, reason);
    
    /* Deactivate previous toplevel */
    struct wlr_surface *prev = SEAT(fm)->keyboard_state.focused_surface;
    if (prev) {
        struct toplevel *prev_tl = focus_toplevel_from_surface(fm, prev);
        if (prev_tl && prev_tl->xdg)
            wlr_xdg_toplevel_set_activated(prev_tl->xdg, false);
    }
    
    /* Raise and activate new */
    wlr_scene_node_raise_to_top(&tl->scene_tree->node);
    wl_list_remove(&tl->link);
    wl_list_insert(&fm->server->toplevels, &tl->link);
    wlr_xdg_toplevel_set_activated(tl->xdg, true);
    focus_keyboard_set(fm, surface, reason);
}

struct toplevel *focus_get_focused_toplevel(struct focus_manager *fm) {
    struct wlr_surface *kb_focus = SEAT(fm)->keyboard_state.focused_surface;
    return kb_focus ? focus_toplevel_from_surface(fm, kb_focus) : NULL;
}

/* ============== Popup Management ============== */

/* Called when popup is created - add to stack */
void focus_popup_register(struct focus_manager *fm, struct popup_data *pd) {
    wl_list_insert(&fm->popup_stack, &pd->link);
    wlr_log(WLR_DEBUG, "Popup registered, depth=%d", wl_list_length(&fm->popup_stack));
}

/* Called when popup maps - set focus if grabbed */
void focus_popup_mapped(struct focus_manager *fm, struct popup_data *pd) {
    wlr_log(WLR_INFO, "Popup mapped: grabbed=%d", pd->has_grab);
    
    if (pd->has_grab && pd->popup && pd->popup->base && pd->popup->base->surface)
        focus_keyboard_set(fm, pd->popup->base->surface, FOCUS_REASON_POPUP_GRAB);
    
    focus_pointer_recheck(fm);
}

/* Called when popup unmaps */
void focus_popup_unmapped(struct focus_manager *fm, struct popup_data *pd) {
    struct wlr_surface *focused = SEAT(fm)->pointer_state.focused_surface;
    
    if (focused == pd->surface) {
        struct wlr_surface *target = find_fallback_focus(fm, pd->surface);
        if (target) {
            double sx = CURSOR(fm)->x, sy = CURSOR(fm)->y;
            focus_pointer_set(fm, target, sx, sy, FOCUS_REASON_SURFACE_UNMAP);
        } else {
            focus_pointer_set(fm, NULL, 0, 0, FOCUS_REASON_EXPLICIT);
        }
    }
}

/* Called when popup is destroyed - restore focus */
void focus_popup_unregister(struct focus_manager *fm, struct popup_data *pd) {
    bool was_grabbed = pd->has_grab;
    struct wlr_surface *pd_surface = pd->surface;
    
    wlr_log(WLR_INFO, "Popup unregister: grabbed=%d", was_grabbed);
    
    wl_list_remove(&pd->link);
    wl_list_init(&pd->link);
    
    /* Clear pointer focus momentarily */
    wlr_seat_pointer_notify_clear_focus(SEAT(fm));
    wlr_seat_pointer_notify_frame(SEAT(fm));
    
    /* Find new focus target */
    struct wlr_surface *target = find_fallback_focus(fm, pd_surface);
    
    if (target) {
        double sx = CURSOR(fm)->x, sy = CURSOR(fm)->y;
        wlr_seat_pointer_notify_enter(SEAT(fm), target, sx, sy);
        wlr_seat_pointer_notify_frame(SEAT(fm));
        
        if (was_grabbed)
            focus_keyboard_set(fm, target, FOCUS_REASON_POPUP_DISMISS);
    }
    
    /* Re-activate toplevel if no more popups */
    if (wl_list_empty(&fm->popup_stack) && !wl_list_empty(&fm->server->toplevels)) {
        struct toplevel *tl = wl_container_of(fm->server->toplevels.next, tl, link);
        if (tl->xdg) {
            wlr_xdg_toplevel_set_activated(tl->xdg, true);
            if (!was_grabbed && tl->surface)
                focus_keyboard_set(fm, tl->surface, FOCUS_REASON_POPUP_DISMISS);
        }
    }
}

struct popup_data *focus_popup_get_topmost(struct focus_manager *fm) {
    if (wl_list_empty(&fm->popup_stack)) return NULL;
    return wl_container_of(fm->popup_stack.next, (struct popup_data *)NULL, link);
}

struct popup_data *focus_popup_from_surface(struct focus_manager *fm,
                                             struct wlr_surface *surface) {
    struct popup_data *pd;
    wl_list_for_each(pd, &fm->popup_stack, link) {
        if (pd->surface == surface) return pd;
    }
    
    /* Check subsurface chain */
    struct wlr_surface *check = surface;
    while (check) {
        struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(check);
        if (!sub) break;
        check = sub->parent;
        wl_list_for_each(pd, &fm->popup_stack, link) {
            if (pd->surface == check) return pd;
        }
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
    struct popup_data *pd = focus_popup_get_topmost(fm);
    if (pd && pd->has_grab) {
        wlr_log(WLR_INFO, "Dismissing grabbed popup via Escape");
        wlr_xdg_popup_destroy(pd->popup);
        return true;
    }
    return false;
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
    if (SEAT(fm)->pointer_state.focused_surface == surface) {
        struct wlr_surface *target = find_fallback_focus(fm, surface);
        if (target)
            focus_pointer_set(fm, target, CURSOR(fm)->x, CURSOR(fm)->y,
                              FOCUS_REASON_SURFACE_UNMAP);
        else
            focus_pointer_set(fm, NULL, 0, 0, FOCUS_REASON_EXPLICIT);
    }
    
    if (SEAT(fm)->keyboard_state.focused_surface == surface) {
        struct wlr_surface *target = find_fallback_focus(fm, surface);
        if (target)
            focus_keyboard_set(fm, target, FOCUS_REASON_SURFACE_UNMAP);
        else
            focus_keyboard_set(fm, NULL, FOCUS_REASON_EXPLICIT);
    }
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
    
    /* Click on popup - keep it focused */
    if (focus_popup_from_surface(fm, clicked))
        return clicked;
    
    /* Click outside popup stack - dismiss all */
    if (!focus_popup_stack_empty(fm)) {
        wlr_log(WLR_INFO, "Click outside popups, dismissing");
        focus_popup_dismiss_all(fm);
        
        struct wlr_surface *new_surface = focus_surface_at_cursor(fm, &sx, &sy);
        if (new_surface)
            focus_pointer_set(fm, new_surface, sx, sy, FOCUS_REASON_POINTER_CLICK);
        return new_surface;
    }
    
    /* Click on toplevel */
    struct toplevel *tl = focus_toplevel_at_cursor(fm);
    if (!tl) tl = focus_toplevel_from_surface(fm, clicked);
    if (tl) focus_toplevel(fm, tl, FOCUS_REASON_POINTER_CLICK);
    
    return clicked;
}

/* focus_phys_to_logical is defined as static inline in focus_manager.h */

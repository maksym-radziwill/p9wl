/*
 * focus_manager.c - Unified focus state machine implementation
 *
 * Consolidates focus logic from focus.c, popup.c, and wl_input.c
 * into a single coherent state machine.
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

#include "types.h"  /* Includes focus_manager.h, defines struct server/toplevel */

/* ============== Internal Helpers ============== */

/*
 * Get the wlr_seat from focus manager.
 */
static inline struct wlr_seat *fm_seat(struct focus_manager *fm) {
    return fm->server->seat;
}

/*
 * Get the wlr_cursor from focus manager.
 */
static inline struct wlr_cursor *fm_cursor(struct focus_manager *fm) {
    return fm->server->cursor;
}

/*
 * Check if any pointer buttons are currently held.
 */
static inline bool fm_buttons_held(struct focus_manager *fm) {
    return fm_seat(fm)->pointer_state.button_count > 0;
}

/*
 * Send keyboard enter to a surface with current state.
 */
static void fm_send_keyboard_enter(struct focus_manager *fm, struct wlr_surface *surface) {
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(fm_seat(fm));
    if (kb && surface) {
        wlr_seat_keyboard_notify_enter(fm_seat(fm), surface,
            kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
}

/*
 * Find the best focus target after a surface goes away.
 * Priority:
 * 1. Parent popup (if in popup stack)
 * 2. First mapped toplevel
 * 3. NULL
 */
static struct wlr_surface *fm_find_fallback_focus(
    struct focus_manager *fm,
    struct wlr_surface *excluding
) {
    /* Check popup stack for a valid target */
    struct popup_data *pd;
    wl_list_for_each(pd, &fm->popup_stack, link) {
        if (pd->surface != excluding && 
            pd->mapped && 
            pd->popup->base && 
            pd->popup->base->surface &&
            pd->popup->base->surface->mapped) {
            return pd->popup->base->surface;
        }
    }
    
    /* Fall back to first mapped toplevel */
    if (!wl_list_empty(&fm->server->toplevels)) {
        struct toplevel *tl;
        wl_list_for_each(tl, &fm->server->toplevels, link) {
            if (tl->surface != excluding &&
                tl->xdg && tl->xdg->base && 
                tl->xdg->base->surface &&
                tl->xdg->base->surface->mapped) {
                return tl->xdg->base->surface;
            }
        }
    }
    
    return NULL;
}

/*
 * Compute surface-local coordinates for a popup.
 */
static void fm_popup_surface_coords(
    struct focus_manager *fm,
    struct popup_data *pd,
    double *out_sx, double *out_sy
) {
    int px = 0, py = 0;
    wlr_scene_node_coords(&pd->scene_tree->node, &px, &py);
    struct wlr_box geo = pd->popup->current.geometry;
    *out_sx = fm_cursor(fm)->x - px - geo.x;
    *out_sy = fm_cursor(fm)->y - py - geo.y;
}

/* ============== Initialization ============== */

void focus_manager_init(struct focus_manager *fm, struct server *server) {
    memset(fm, 0, sizeof(*fm));
    fm->server = server;
    wl_list_init(&fm->popup_stack);
    
    wlr_log(WLR_INFO, "Focus manager initialized");
}

void focus_manager_cleanup(struct focus_manager *fm) {
    /* Popups should already be cleaned up by their destroy handlers */
    wlr_log(WLR_INFO, "Focus manager cleanup (handled %d focus changes)", 
            fm->focus_change_count);
}

/* ============== Surface Queries ============== */

struct wlr_surface *focus_surface_at_cursor(
    struct focus_manager *fm,
    double *out_sx,
    double *out_sy
) {
    struct server *s = fm->server;
    double sx, sy;
    struct wlr_surface *surface = NULL;
    
    struct wlr_scene_node *node = wlr_scene_node_at(
        &s->scene->tree.node,
        fm_cursor(fm)->x, fm_cursor(fm)->y,
        &sx, &sy
    );
    
    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
        if (ss && ss->surface && ss->surface->mapped) {
            surface = ss->surface;
            *out_sx = sx;
            *out_sy = sy;
        }
    }
    
    /* Fallback to first mapped toplevel if node exists but no surface */
    if (!surface && node && !wl_list_empty(&s->toplevels)) {
        struct toplevel *tl = wl_container_of(s->toplevels.next, tl, link);
        if (tl->xdg && tl->xdg->base && tl->xdg->base->surface &&
            tl->xdg->base->surface->mapped) {
            surface = tl->xdg->base->surface;
            *out_sx = fm_cursor(fm)->x;
            *out_sy = fm_cursor(fm)->y;
        }
    }
    
    return surface;
}

struct toplevel *focus_toplevel_from_surface(
    struct focus_manager *fm,
    struct wlr_surface *surface
) {
    if (!surface) return NULL;
    
    struct server *s = fm->server;
    struct toplevel *tl;
    
    wl_list_for_each(tl, &s->toplevels, link) {
        /* Direct match */
        if (tl->xdg && tl->xdg->base && tl->xdg->base->surface == surface) {
            return tl;
        }
        
        /* Check subsurface chain */
        struct wlr_surface *check = surface;
        while (check) {
            struct wlr_subsurface *subsurface = 
                wlr_subsurface_try_from_wlr_surface(check);
            if (!subsurface) break;
            check = subsurface->parent;
            if (tl->xdg && tl->xdg->base && tl->xdg->base->surface == check) {
                return tl;
            }
        }
    }
    
    return NULL;
}

struct toplevel *focus_toplevel_from_node(
    struct focus_manager *fm,
    struct wlr_scene_node *node
) {
    (void)fm;  /* May be used for validation in future */
    
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

struct toplevel *focus_toplevel_at_cursor(struct focus_manager *fm) {
    struct server *s = fm->server;
    double sx, sy;
    
    struct wlr_scene_node *node = wlr_scene_node_at(
        &s->scene->tree.node,
        fm_cursor(fm)->x, fm_cursor(fm)->y,
        &sx, &sy
    );
    
    if (!node) return NULL;
    
    /* Try to get toplevel from node data */
    struct toplevel *tl = focus_toplevel_from_node(fm, node);
    if (tl) return tl;
    
    /* Fallback: get surface and find its toplevel */
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
        if (ss && ss->surface) {
            return focus_toplevel_from_surface(fm, ss->surface);
        }
    }
    
    return NULL;
}

/* ============== Pointer Focus ============== */

void focus_pointer_set(
    struct focus_manager *fm,
    struct wlr_surface *surface,
    double sx, double sy,
    enum focus_reason reason
) {
    struct wlr_seat *seat = fm_seat(fm);
    struct wlr_surface *current = seat->pointer_state.focused_surface;
    
    /* No change needed */
    if (surface == current) {
        return;
    }
    
    /* Defer focus change if buttons are held (except for explicit/destroy) */
    if (fm_buttons_held(fm) && 
        reason != FOCUS_REASON_EXPLICIT &&
        reason != FOCUS_REASON_SURFACE_DESTROY) {
        fm->pointer_focus_deferred = true;
        fm->deferred_pointer_target = surface;
        fm->deferred_sx = sx;
        fm->deferred_sy = sy;
        wlr_log(WLR_DEBUG, "Focus: pointer change deferred (buttons held)");
        return;
    }
    
    fm->focus_change_count++;
    fm->pointer_focus = surface;
    
    if (surface) {
        wlr_log(WLR_DEBUG, "Focus: pointer enter surface %p at (%.1f, %.1f) reason=%d",
                (void*)surface, sx, sy, reason);
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    } else {
        wlr_log(WLR_DEBUG, "Focus: pointer clear reason=%d", reason);
        wlr_seat_pointer_notify_clear_focus(seat);
    }
    wlr_seat_pointer_notify_frame(seat);
}

void focus_pointer_motion(
    struct focus_manager *fm,
    double sx, double sy,
    uint32_t time_msec
) {
    wlr_seat_pointer_notify_motion(fm_seat(fm), time_msec, sx, sy);
}

void focus_pointer_clear(struct focus_manager *fm) {
    focus_pointer_set(fm, NULL, 0, 0, FOCUS_REASON_EXPLICIT);
}

void focus_pointer_recheck(struct focus_manager *fm) {
    /* Handle deferred focus first */
    if (fm->pointer_focus_deferred && !fm_buttons_held(fm)) {
        fm->pointer_focus_deferred = false;
        if (fm->deferred_pointer_target) {
            focus_pointer_set(fm, fm->deferred_pointer_target,
                              fm->deferred_sx, fm->deferred_sy,
                              FOCUS_REASON_POINTER_MOTION);
            fm->deferred_pointer_target = NULL;
        }
        return;
    }
    
    /* Don't change focus during drags */
    if (fm_buttons_held(fm)) {
        fm->pointer_focus_deferred = true;
        return;
    }
    
    /* Query current surface under cursor */
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    struct wlr_surface *focused = fm_seat(fm)->pointer_state.focused_surface;
    
    if (surface != focused) {
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
    }
}

void focus_pointer_button_pressed(struct focus_manager *fm) {
    /* Button count is tracked by seat, we just note that changes are deferred */
    (void)fm;
}

void focus_pointer_button_released(struct focus_manager *fm) {
    /* Check if we need to apply deferred focus */
    if (fm->pointer_focus_deferred && !fm_buttons_held(fm)) {
        focus_pointer_recheck(fm);
    }
}

/* ============== Keyboard Focus ============== */

void focus_keyboard_set(
    struct focus_manager *fm,
    struct wlr_surface *surface,
    enum focus_reason reason
) {
    struct wlr_seat *seat = fm_seat(fm);
    struct wlr_surface *current = seat->keyboard_state.focused_surface;
    
    if (surface == current) {
        return;
    }
    
    fm->keyboard_focus = surface;
    fm->focus_change_count++;
    
    if (surface) {
        wlr_log(WLR_DEBUG, "Focus: keyboard enter surface %p reason=%d",
                (void*)surface, reason);
        fm_send_keyboard_enter(fm, surface);
    } else {
        wlr_log(WLR_DEBUG, "Focus: keyboard clear reason=%d", reason);
        wlr_seat_keyboard_notify_clear_focus(seat);
    }
}

void focus_keyboard_clear(struct focus_manager *fm) {
    focus_keyboard_set(fm, NULL, FOCUS_REASON_EXPLICIT);
}

void focus_keyboard_set_modifiers(struct focus_manager *fm, uint32_t modifiers) {
    fm->modifier_state = modifiers;
    struct wlr_keyboard_modifiers mods = { .depressed = modifiers };
    wlr_seat_keyboard_notify_modifiers(fm_seat(fm), &mods);
}

uint32_t focus_keyboard_get_modifiers(struct focus_manager *fm) {
    return fm->modifier_state;
}

/* ============== Toplevel Focus ============== */

void fm_focus_toplevel(
    struct focus_manager *fm,
    struct toplevel *tl,
    enum focus_reason reason
) {
    if (!tl || !tl->xdg || !tl->xdg->base || !tl->xdg->base->surface) {
        return;
    }
    
    struct server *s = fm->server;
    struct wlr_surface *surface = tl->xdg->base->surface;
    struct wlr_surface *prev_surface = fm_seat(fm)->keyboard_state.focused_surface;
    
    if (prev_surface == surface) {
        return;
    }
    
    wlr_log(WLR_INFO, "Focus: toplevel %p reason=%d", (void*)tl, reason);
    
    /* Deactivate previously focused toplevel */
    if (prev_surface) {
        struct toplevel *prev_tl = focus_toplevel_from_surface(fm, prev_surface);
        if (prev_tl && prev_tl->xdg) {
            wlr_xdg_toplevel_set_activated(prev_tl->xdg, false);
        }
    }
    
    /* Raise scene node */
    wlr_scene_node_raise_to_top(&tl->scene_tree->node);
    
    /* Move toplevel to front of list */
    wl_list_remove(&tl->link);
    wl_list_insert(&s->toplevels, &tl->link);
    
    /* Activate new toplevel */
    wlr_xdg_toplevel_set_activated(tl->xdg, true);
    
    /* Set keyboard focus */
    focus_keyboard_set(fm, surface, reason);
}

struct toplevel *focus_get_focused_toplevel(struct focus_manager *fm) {
    struct wlr_surface *kb_focus = fm_seat(fm)->keyboard_state.focused_surface;
    if (!kb_focus) return NULL;
    return focus_toplevel_from_surface(fm, kb_focus);
}

/* ============== Popup Management ============== */

void focus_popup_register(struct focus_manager *fm, struct popup_data *pd) {
    /* Add to front of popup stack */
    wl_list_insert(&fm->popup_stack, &pd->link);
    wlr_log(WLR_INFO, "Focus: popup registered, stack depth=%d",
            wl_list_length(&fm->popup_stack));
}

void focus_popup_unregister(struct focus_manager *fm, struct popup_data *pd) {
    bool was_grabbed = pd->has_grab;
    struct wlr_surface *pd_surface = pd->surface;
    struct wlr_surface *focused_ptr = fm_seat(fm)->pointer_state.focused_surface;
    bool had_pointer_focus = (focused_ptr == pd_surface);
    
    wlr_log(WLR_INFO, "Focus: popup unregister surface=%p grabbed=%d ptr_focus=%d",
            (void*)pd_surface, was_grabbed, had_pointer_focus);
    
    /* Remove from stack */
    wl_list_remove(&pd->link);
    wl_list_init(&pd->link);  /* Prevent double-remove */
    
    /* Clear pointer focus first - the surface is about to be invalid */
    wlr_seat_pointer_notify_clear_focus(fm_seat(fm));
    wlr_seat_pointer_notify_frame(fm_seat(fm));
    
    /* Find new focus target */
    struct wlr_surface *target = fm_find_fallback_focus(fm, pd_surface);
    
    if (target) {
        double sx, sy;
        
        /* Check if target is a popup */
        struct popup_data *target_pd = focus_popup_from_surface(fm, target);
        if (target_pd) {
            fm_popup_surface_coords(fm, target_pd, &sx, &sy);
        } else {
            sx = fm_cursor(fm)->x;
            sy = fm_cursor(fm)->y;
        }
        
        /* Send pointer enter and motion */
        wlr_seat_pointer_notify_enter(fm_seat(fm), target, sx, sy);
        wlr_seat_pointer_notify_frame(fm_seat(fm));
        wlr_seat_pointer_notify_motion(fm_seat(fm), now_ms(), sx, sy);
        wlr_seat_pointer_notify_frame(fm_seat(fm));
        
        wlr_log(WLR_DEBUG, "Focus: pointer transferred to %p at (%.1f, %.1f)",
                (void*)target, sx, sy);
    }
    
    /* Handle keyboard focus transfer for grabbed popups */
    if (was_grabbed) {
        struct popup_data *parent_popup = focus_popup_get_topmost(fm);
        
        if (parent_popup && parent_popup->has_grab &&
            parent_popup->popup->base && parent_popup->popup->base->surface &&
            parent_popup->popup->base->surface->mapped) {
            wlr_log(WLR_DEBUG, "Focus: keyboard grab to parent popup");
            focus_keyboard_set(fm, parent_popup->popup->base->surface,
                               FOCUS_REASON_POPUP_DISMISS);
        } else if (target && !focus_popup_from_surface(fm, target)) {
            /* Target is a toplevel */
            wlr_log(WLR_DEBUG, "Focus: keyboard to toplevel after popup dismiss");
            focus_keyboard_set(fm, target, FOCUS_REASON_POPUP_DISMISS);
        }
    }
    
    /* Re-activate toplevel if no more popups */
    if (wl_list_empty(&fm->popup_stack) && !wl_list_empty(&fm->server->toplevels)) {
        struct toplevel *tl = wl_container_of(fm->server->toplevels.next, tl, link);
        if (tl->xdg) {
            wlr_xdg_toplevel_set_activated(tl->xdg, true);
            if (!was_grabbed && tl->xdg->base && tl->xdg->base->surface) {
                focus_keyboard_set(fm, tl->xdg->base->surface,
                                   FOCUS_REASON_POPUP_DISMISS);
            }
        }
    }
}

void focus_popup_mapped(struct focus_manager *fm, struct popup_data *pd) {
    wlr_log(WLR_INFO, "Focus: popup mapped surface=%p grabbed=%d",
            (void*)pd->surface, pd->has_grab);
    
    if (pd->has_grab && pd->popup->base && pd->popup->base->surface) {
        focus_keyboard_set(fm, pd->popup->base->surface, FOCUS_REASON_POPUP_GRAB);
    }
    
    focus_pointer_recheck(fm);
}

void focus_popup_unmapped(struct focus_manager *fm, struct popup_data *pd) {
    struct wlr_surface *focused = fm_seat(fm)->pointer_state.focused_surface;
    
    wlr_log(WLR_DEBUG, "Focus: popup unmapped surface=%p", (void*)pd->surface);
    
    if (focused == pd->surface) {
        /* Find a new pointer target */
        struct wlr_surface *target = fm_find_fallback_focus(fm, pd->surface);
        if (target) {
            double sx, sy;
            struct popup_data *target_pd = focus_popup_from_surface(fm, target);
            if (target_pd) {
                fm_popup_surface_coords(fm, target_pd, &sx, &sy);
            } else {
                sx = fm_cursor(fm)->x;
                sy = fm_cursor(fm)->y;
            }
            focus_pointer_set(fm, target, sx, sy, FOCUS_REASON_SURFACE_UNMAP);
        } else {
            focus_pointer_clear(fm);
        }
    }
}

struct popup_data *focus_popup_get_topmost(struct focus_manager *fm) {
    if (wl_list_empty(&fm->popup_stack)) return NULL;
    struct popup_data *pd;
    return wl_container_of(fm->popup_stack.next, pd, link);
}

struct popup_data *focus_popup_from_surface(
    struct focus_manager *fm,
    struct wlr_surface *surface
) {
    struct popup_data *pd;
    
    /* Direct match */
    wl_list_for_each(pd, &fm->popup_stack, link) {
        if (pd->surface == surface) return pd;
    }
    
    /* Check subsurface chain */
    struct wlr_surface *check = surface;
    while (check) {
        struct wlr_subsurface *subsurface = 
            wlr_subsurface_try_from_wlr_surface(check);
        if (!subsurface) break;
        check = subsurface->parent;
        
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
        wlr_log(WLR_INFO, "Focus: dismissing grabbed popup via Escape");
        wlr_xdg_popup_destroy(pd->popup);
        return true;
    }
    return false;
}

bool focus_popup_stack_empty(struct focus_manager *fm) {
    return wl_list_empty(&fm->popup_stack);
}

/* ============== Surface Lifecycle ============== */

void focus_on_surface_map(
    struct focus_manager *fm,
    struct wlr_surface *surface,
    bool is_toplevel
) {
    wlr_log(WLR_DEBUG, "Focus: surface map %p is_toplevel=%d",
            (void*)surface, is_toplevel);
    
    if (is_toplevel) {
        struct toplevel *tl = focus_toplevel_from_surface(fm, surface);
        if (tl) {
            fm_focus_toplevel(fm, tl, FOCUS_REASON_SURFACE_MAP);
        }
    }
    
    /* Check if cursor is now over this surface */
    double sx, sy;
    struct wlr_surface *under = focus_surface_at_cursor(fm, &sx, &sy);
    if (under == surface) {
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_SURFACE_MAP);
    }
}

void focus_on_surface_unmap(
    struct focus_manager *fm,
    struct wlr_surface *surface
) {
    wlr_log(WLR_DEBUG, "Focus: surface unmap %p", (void*)surface);
    
    /* Clear pointer focus if needed */
    if (fm_seat(fm)->pointer_state.focused_surface == surface) {
        struct wlr_surface *target = fm_find_fallback_focus(fm, surface);
        if (target) {
            double sx = fm_cursor(fm)->x;
            double sy = fm_cursor(fm)->y;
            focus_pointer_set(fm, target, sx, sy, FOCUS_REASON_SURFACE_UNMAP);
        } else {
            focus_pointer_clear(fm);
        }
    }
    
    /* Clear keyboard focus if needed */
    if (fm_seat(fm)->keyboard_state.focused_surface == surface) {
        struct wlr_surface *target = fm_find_fallback_focus(fm, surface);
        if (target) {
            focus_keyboard_set(fm, target, FOCUS_REASON_SURFACE_UNMAP);
        } else {
            focus_keyboard_clear(fm);
        }
    }
}

void focus_on_surface_destroy(
    struct focus_manager *fm,
    struct wlr_surface *surface
) {
    wlr_log(WLR_DEBUG, "Focus: surface destroy %p", (void*)surface);
    
    /* Same as unmap but more urgent */
    focus_on_surface_unmap(fm, surface);
    
    /* Clear any deferred focus to this surface */
    if (fm->deferred_pointer_target == surface) {
        fm->deferred_pointer_target = NULL;
        fm->pointer_focus_deferred = false;
    }
}

/* ============== Click Handling ============== */

struct wlr_surface *focus_handle_click(
    struct focus_manager *fm,
    struct wlr_surface *clicked_surface,
    double sx, double sy,
    uint32_t button
) {
    (void)button;  /* Could differentiate behavior by button in future */
    
    /* Check if click is on a popup */
    struct popup_data *clicked_popup = 
        focus_popup_from_surface(fm, clicked_surface);
    
    if (clicked_popup) {
        /* Click on popup - no special handling needed */
        return clicked_surface;
    }
    
    /* Click outside popup stack - dismiss all popups */
    if (!focus_popup_stack_empty(fm)) {
        wlr_log(WLR_INFO, "Focus: click outside popups, dismissing all");
        focus_popup_dismiss_all(fm);
        
        /* Re-query surface after popups are gone */
        struct wlr_surface *new_surface = focus_surface_at_cursor(fm, &sx, &sy);
        if (new_surface) {
            focus_pointer_set(fm, new_surface, sx, sy, FOCUS_REASON_POINTER_CLICK);
        }
        return new_surface;
    }
    
    /* Click on toplevel - focus it */
    struct toplevel *clicked_tl = focus_toplevel_at_cursor(fm);
    if (!clicked_tl) {
        clicked_tl = focus_toplevel_from_surface(fm, clicked_surface);
    }
    
    if (clicked_tl) {
        fm_focus_toplevel(fm, clicked_tl, FOCUS_REASON_POINTER_CLICK);
    }
    
    return clicked_surface;
}

/* now_ms() is defined as static inline in types.h */

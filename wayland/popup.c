/*
 * popup.c - Popup management (refactored)
 *
 * Handles XDG popup lifecycle. Focus-related logic has been
 * consolidated into focus_manager.c
 */

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "popup.h"
#include "../types.h"  /* Includes focus_manager.h */

/* Forward declaration */
extern uint32_t now_ms(void);

/* ============== Legacy API Wrappers ============== */

struct popup_data *get_topmost_popup(struct server *s) {
    return focus_popup_get_topmost(&s->focus);
}

struct popup_data *find_popup_by_surface(struct server *s, struct wlr_surface *surface) {
    return focus_popup_from_surface(&s->focus, surface);
}

void dismiss_all_popups(struct server *s) {
    focus_popup_dismiss_all(&s->focus);
}

bool dismiss_topmost_grabbed_popup(struct server *s) {
    return focus_popup_dismiss_topmost_grabbed(&s->focus);
}

/* ============== Popup Lifecycle Handlers ============== */

static void popup_destroy(struct wl_listener *l, void *d) {
    struct popup_data *pd = wl_container_of(l, pd, destroy);
    (void)d;
    
    struct server *s = pd->server;
    
    wlr_log(WLR_INFO, "Popup DESTROYED: surface=%p", pd->surface);
    
    /* Delegate focus handling to focus_manager */
    focus_popup_unregister(&s->focus, pd);
    
    /* Cleanup listeners */
    wl_list_remove(&pd->commit.link);
    wl_list_remove(&pd->destroy.link);
    if (!wl_list_empty(&pd->grab.link)) {
        wl_list_remove(&pd->grab.link);
    }
    
    free(pd);
    
    wlr_log(WLR_INFO, "Popup destruction complete");
}

static void popup_commit(struct wl_listener *l, void *d) {
    struct popup_data *pd = wl_container_of(l, pd, commit);
    (void)d;
    
    struct wlr_xdg_popup *popup = pd->popup;
    struct wlr_surface *surface = popup->base->surface;
    struct server *s = pd->server;
    
    if (popup->base->initial_commit) {
        /* For 9front scaling mode, s->width/height are already logical.
         * For Wayland scaling or no scaling, convert physical to logical.
         */
        int logical_w, logical_h;
        if (!s->wl_scaling && s->scale > 1.001f) {
            /* 9front scaling: s->width is already logical */
            logical_w = s->width;
            logical_h = s->height;
        } else {
            /* Wayland scaling or no scaling */
            logical_w = focus_phys_to_logical(s->width, s->scale);
            logical_h = focus_phys_to_logical(s->height, s->scale);
        }
        struct wlr_box box = {
            .x = 0,
            .y = 0,
            .width = logical_w,
            .height = logical_h,
        };
        wlr_xdg_popup_unconstrain_from_box(popup, &box);
        pd->configured = 1;
        wlr_log(WLR_INFO, "Popup initial commit: unconstrained to %dx%d", 
                box.width, box.height);
        return;
    }
    
    if (!surface->mapped) {
        return;
    }
    
    pd->commit_count++;
    bool has_buffer = wlr_surface_has_buffer(surface);
    
    /* Track map/unmap state changes */
    if (has_buffer && !pd->mapped) {
        pd->mapped = true;
        wlr_log(WLR_INFO, "Popup MAPPED: surface=%p has_grab=%d", pd->surface, pd->has_grab);
        focus_popup_mapped(&s->focus, pd);
    } else if (!has_buffer && pd->mapped) {
        pd->mapped = false;
        wlr_log(WLR_INFO, "Popup UNMAPPED: surface=%p", pd->surface);
        focus_popup_unmapped(&s->focus, pd);
    }
    
    if (pd->commit_count <= 10 || pd->commit_count % 50 == 0) {
        struct wlr_box geo = popup->current.geometry;
        wlr_log(WLR_DEBUG, "Popup %p commit #%d: geo=(%d,%d %dx%d) mapped=%d",
                (void*)pd->popup, pd->commit_count, geo.x, geo.y, 
                geo.width, geo.height, pd->mapped);
    }
}

void new_popup(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_xdg_popup);
    struct wlr_xdg_popup *popup = d;
    
    wlr_log(WLR_INFO, "New XDG popup created, parent=%p", (void*)popup->parent);
    
    /* Find parent scene tree */
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (!parent) {
        wlr_log(WLR_ERROR, "Popup: wlr_xdg_surface_try_from_wlr_surface failed");
        return;
    }
    if (!parent->data) {
        wlr_log(WLR_ERROR, "Popup: parent has no scene tree data");
        return;
    }
    
    struct wlr_scene_tree *parent_tree = parent->data;
    
    /* Create scene tree for popup */
    struct wlr_scene_tree *popup_tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
    if (!popup_tree) {
        wlr_log(WLR_ERROR, "Failed to create popup scene tree");
        return;
    }
    popup->base->data = popup_tree;
    
    /* Allocate popup tracking data */
    struct popup_data *pd = calloc(1, sizeof(*pd));
    if (!pd) {
        wlr_log(WLR_ERROR, "Failed to allocate popup_data");
        return;
    }
    
    pd->popup = popup;
    pd->surface = popup->base->surface;
    pd->scene_tree = popup_tree;
    pd->server = s;
    pd->configured = 0;
    pd->commit_count = 0;
    pd->has_grab = (popup->seat != NULL);
    pd->mapped = false;
    wl_list_init(&pd->grab.link);
    wl_list_init(&pd->link);
    
    /* Register with focus manager (adds to popup stack) */
    focus_popup_register(&s->focus, pd);
    
    /* Setup listeners */
    pd->commit.notify = popup_commit;
    wl_signal_add(&popup->base->surface->events.commit, &pd->commit);
    
    pd->destroy.notify = popup_destroy;
    wl_signal_add(&popup->base->events.destroy, &pd->destroy);
    
    wlr_log(WLR_INFO, "Popup scene tree created at %p (has_grab=%d)", 
            (void*)popup_tree, pd->has_grab);
}

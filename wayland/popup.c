/*
 * popup.c - Popup lifecycle and subsurface tracking
 *
 * Handles XDG popup creation, commit, destruction, and tracks
 * subsurfaces that appear on popup surfaces (e.g. nested widgets).
 * Focus logic is handled by focus_manager.c
 */

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include "popup.h"
#include "../types.h"

/* Iterate both below and above subsurface lists */
#define FOR_EACH_SUBSURFACE(surface, sub) \
    for (int _list_idx = 0; _list_idx < 2; _list_idx++) \
        wl_list_for_each(sub, \
            (_list_idx == 0) ? &(surface)->current.subsurfaces_below \
                             : &(surface)->current.subsurfaces_above, \
            current.link)

/* ============== Subsurface Tracking ============== */

/*
 * Popup-local subsurface tracker.  Parallel to toplevel's
 * subsurface_track but references popup_data instead of toplevel.
 */
struct popup_sub {
    struct wlr_subsurface *subsurface;
    struct server *server;
    struct popup_data *popup;
    bool mapped;
    struct wl_listener destroy;
    struct wl_listener commit;
    struct wl_list link;  /* popup_data.subsurfaces */
};

static void popup_sub_commit(struct wl_listener *l, void *d) {
    struct popup_sub *ps = wl_container_of(l, ps, commit);
    (void)d;

    struct wlr_surface *surface = ps->subsurface->surface;
    bool has_buffer = wlr_surface_has_buffer(surface);

    if (has_buffer != ps->mapped) {
        ps->mapped = has_buffer;
        focus_pointer_recheck(&ps->server->focus);
    }

    /*
     * No mark_surface_dirty_tiles here — the popup's absolute
     * position isn't cheaply available.  scene_dirty is enough;
     * the send thread will detect the changed tiles.
     */
    ps->server->scene_dirty = 1;
    wlr_output_schedule_frame(ps->server->output);
}

static void popup_sub_destroy(struct wl_listener *l, void *d) {
    struct popup_sub *ps = wl_container_of(l, ps, destroy);
    (void)d;

    wl_list_remove(&ps->destroy.link);
    wl_list_remove(&ps->commit.link);
    wl_list_remove(&ps->link);
    free(ps);
}

static bool is_sub_tracked(struct popup_data *pd, struct wlr_subsurface *sub) {
    struct popup_sub *ps;
    wl_list_for_each(ps, &pd->subsurfaces, link) {
        if (ps->subsurface == sub) return true;
    }
    return false;
}

static void track_sub(struct popup_data *pd, struct wlr_subsurface *sub) {
    struct popup_sub *ps = calloc(1, sizeof(*ps));
    if (!ps) return;

    ps->subsurface = sub;
    ps->server = pd->server;
    ps->popup = pd;

    ps->destroy.notify = popup_sub_destroy;
    wl_signal_add(&sub->events.destroy, &ps->destroy);

    ps->commit.notify = popup_sub_commit;
    wl_signal_add(&sub->surface->events.commit, &ps->commit);

    wl_list_insert(&pd->subsurfaces, &ps->link);
}

static void check_new_subsurfaces(struct popup_data *pd) {
    struct wlr_surface *surface = pd->popup->base->surface;
    struct wlr_subsurface *sub;

    FOR_EACH_SUBSURFACE(surface, sub) {
        if (!is_sub_tracked(pd, sub))
            track_sub(pd, sub);
    }
}

/* ============== Popup Handlers ============== */

static void popup_destroy(struct wl_listener *l, void *d) {
    struct popup_data *pd = wl_container_of(l, pd, destroy);
    (void)d;

    struct server *s = pd->server;

    wlr_log(WLR_INFO, "Popup destroyed: surface=%p", pd->surface);

    focus_popup_unregister(&s->focus, pd);

    struct popup_sub *ps, *tmp;
    wl_list_for_each_safe(ps, tmp, &pd->subsurfaces, link) {
        wl_list_remove(&ps->destroy.link);
        wl_list_remove(&ps->commit.link);
        wl_list_remove(&ps->link);
        free(ps);
    }

    wl_list_remove(&pd->commit.link);
    wl_list_remove(&pd->destroy.link);
    free(pd);
}

static void popup_commit(struct wl_listener *l, void *d) {
    struct popup_data *pd = wl_container_of(l, pd, commit);
    (void)d;

    struct wlr_xdg_popup *popup = pd->popup;
    struct wlr_surface *surface = popup->base->surface;
    struct server *s = pd->server;

    if (popup->base->initial_commit) {
        int logical_w = focus_phys_to_logical(s->width, s->scale);
        int logical_h = focus_phys_to_logical(s->height, s->scale);
        struct wlr_box box = { 0, 0, logical_w, logical_h };
        wlr_xdg_popup_unconstrain_from_box(popup, &box);
        pd->configured = 1;
        return;
    }

    if (!surface->mapped) return;

    pd->commit_count++;
    bool has_buffer = wlr_surface_has_buffer(surface);

    if (has_buffer && !pd->mapped) {
        pd->mapped = true;
        wlr_log(WLR_INFO, "Popup mapped: surface=%p grab=%d", pd->surface, pd->has_grab);
        focus_popup_mapped(&s->focus, pd);
    } else if (!has_buffer && pd->mapped) {
        pd->mapped = false;
        wlr_log(WLR_INFO, "Popup unmapped: surface=%p", pd->surface);
        focus_popup_unmapped(&s->focus, pd);
    }

    check_new_subsurfaces(pd);

    s->scene_dirty = 1;
    wlr_output_schedule_frame(s->output);
}

void new_popup(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_xdg_popup);
    struct wlr_xdg_popup *popup = d;

    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (!parent || !parent->data) {
        wlr_log(WLR_ERROR, "Popup: no valid parent surface");
        return;
    }

    struct wlr_scene_tree *popup_tree =
        wlr_scene_xdg_surface_create(parent->data, popup->base);
    if (!popup_tree) return;
    popup->base->data = popup_tree;

    struct popup_data *pd = calloc(1, sizeof(*pd));
    if (!pd) return;

    pd->popup = popup;
    pd->surface = popup->base->surface;
    pd->scene_tree = popup_tree;
    pd->server = s;
    pd->has_grab = (popup->seat != NULL);
    wl_list_init(&pd->subsurfaces);
    wl_list_init(&pd->link);

    focus_popup_register(&s->focus, pd);

    pd->commit.notify = popup_commit;
    wl_signal_add(&popup->base->surface->events.commit, &pd->commit);

    pd->destroy.notify = popup_destroy;
    wl_signal_add(&popup->base->events.destroy, &pd->destroy);

    wlr_log(WLR_INFO, "New popup: parent=%p grab=%d", (void*)popup->parent, pd->has_grab);
}

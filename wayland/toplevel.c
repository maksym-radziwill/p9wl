/*
 * toplevel.c - Toplevel and subsurface handlers
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

#include "toplevel.h"
#include "types.h"
#include "draw/draw_helpers.h"
#include "draw/draw.h"
#include "p9/p9.h"
#include "wayland/output.h"

static void check_new_subsurfaces(struct toplevel *tl);

/* Iterate both below and above subsurface lists */
#define FOR_EACH_SUBSURFACE(surface, sub) \
    for (int _list_idx = 0; _list_idx < 2; _list_idx++) \
        wl_list_for_each(sub, \
            (_list_idx == 0) ? &(surface)->current.subsurfaces_below \
                             : &(surface)->current.subsurfaces_above, \
            current.link)

/* ============== Subsurface Tracking ============== */

static void subsurface_commit(struct wl_listener *l, void *d) {
    struct subsurface_track *st = wl_container_of(l, st, commit);
    (void)d;

    struct wlr_surface *surface = st->subsurface->surface;
    bool has_buffer = wlr_surface_has_buffer(surface);

    if (has_buffer != st->mapped) {
        st->mapped = has_buffer;
        focus_pointer_recheck(&st->server->focus);
    }

    mark_surface_dirty_tiles(st->server, surface,
                              st->subsurface->current.x,
                              st->subsurface->current.y);

    st->server->scene_dirty = 1;
    wlr_output_schedule_frame(st->server->output);
}

static void subsurface_destroy(struct wl_listener *l, void *d) {
    struct subsurface_track *st = wl_container_of(l, st, destroy);
    (void)d;

    wl_list_remove(&st->destroy.link);
    wl_list_remove(&st->commit.link);
    wl_list_remove(&st->link);
    free(st);
}

static bool is_subsurface_tracked(struct toplevel *tl, struct wlr_subsurface *sub) {
    struct subsurface_track *st;
    wl_list_for_each(st, &tl->subsurfaces, link) {
        if (st->subsurface == sub) return true;
    }
    return false;
}

static void track_subsurface(struct toplevel *tl, struct wlr_subsurface *sub) {
    struct subsurface_track *st = calloc(1, sizeof(*st));
    if (!st) return;

    st->subsurface = sub;
    st->server = tl->server;
    st->toplevel = tl;

    st->destroy.notify = subsurface_destroy;
    wl_signal_add(&sub->events.destroy, &st->destroy);

    st->commit.notify = subsurface_commit;
    wl_signal_add(&sub->surface->events.commit, &st->commit);

    wl_list_insert(&tl->subsurfaces, &st->link);
    focus_pointer_recheck(&tl->server->focus);
}

static void check_new_subsurfaces(struct toplevel *tl) {
    struct wlr_surface *surface = tl->xdg->base->surface;
    struct wlr_subsurface *sub;

    FOR_EACH_SUBSURFACE(surface, sub) {
        if (!is_subsurface_tracked(tl, sub))
            track_subsurface(tl, sub);
    }
}

/* ============== Toplevel Handlers ============== */

static void toplevel_commit(struct wl_listener *l, void *d) {
    struct toplevel *tl = wl_container_of(l, tl, commit);
    struct server *s = tl->server;
    struct wlr_xdg_surface *xdg_surface = tl->xdg->base;
    struct wlr_surface *surface = xdg_surface->surface;
    (void)d;

    if (xdg_surface->initial_commit) {
        int logical_w = focus_phys_to_logical(s->width, s->scale);
        int logical_h = focus_phys_to_logical(s->height, s->scale);

        wlr_xdg_toplevel_set_size(tl->xdg, logical_w, logical_h);
        wlr_xdg_toplevel_set_maximized(tl->xdg, true);
        wlr_xdg_toplevel_set_activated(tl->xdg, true);
        wlr_xdg_surface_schedule_configure(xdg_surface);
        tl->configured = true;
        wlr_log(WLR_INFO, "Toplevel configure: %dx%d", logical_w, logical_h);
        return;
    }

    if (!surface->mapped) return;

    tl->commit_count++;
    bool has_buffer = wlr_surface_has_buffer(surface);

    if (has_buffer && !tl->mapped) {
        tl->mapped = true;
        wlr_log(WLR_INFO, "Toplevel mapped");
        focus_on_surface_map(&s->focus, surface, true);

        double sx, sy;
        struct wlr_surface *under = focus_surface_at_cursor(&s->focus, &sx, &sy);
        if (under == surface)
            focus_pointer_set(&s->focus, surface, sx, sy, FOCUS_REASON_SURFACE_MAP);
    } else if (!has_buffer && tl->mapped) {
        tl->mapped = false;
        wlr_log(WLR_INFO, "Toplevel unmapped");
        focus_on_surface_unmap(&s->focus, surface);
    }

    check_new_subsurfaces(tl);
    focus_pointer_recheck(&s->focus);

    mark_surface_dirty_tiles(s, surface, 0.0, 0.0);

    s->scene_dirty = 1;
    wlr_output_schedule_frame(s->output);
}

static void toplevel_destroy(struct wl_listener *l, void *d) {
    struct toplevel *tl = wl_container_of(l, tl, destroy);
    struct server *s = tl->server;
    (void)d;

    wlr_log(WLR_INFO, "Toplevel destroyed");

    focus_on_surface_destroy(&s->focus, tl->surface);

    struct subsurface_track *st, *tmp;
    wl_list_for_each_safe(st, tmp, &tl->subsurfaces, link) {
        wl_list_remove(&st->destroy.link);
        wl_list_remove(&st->commit.link);
        wl_list_remove(&st->link);
        free(st);
    }

    wl_list_remove(&tl->commit.link);
    wl_list_remove(&tl->destroy.link);
    wl_list_remove(&tl->link);
    free(tl);

    if (s->had_toplevel && wl_list_empty(&s->toplevels)) {
        wlr_log(WLR_INFO, "Last toplevel destroyed, shutting down");

        pthread_mutex_lock(&s->send_lock);
        s->running = 0;
        pthread_cond_signal(&s->send_cond);
        pthread_mutex_unlock(&s->send_lock);

        if (s->send_thread) {
            pthread_join(s->send_thread, NULL);
            s->send_thread = 0;
        }

        delete_rio_window(&s->p9_draw);
        p9_disconnect(&s->p9_draw);
        exit(0);
    }
}

void new_toplevel(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg = d;

    s->has_toplevel = 1;
    s->had_toplevel = 1;

    struct toplevel *tl = calloc(1, sizeof(*tl));
    if (!tl) return;

    tl->xdg = xdg;
    tl->server = s;
    tl->surface = xdg->base->surface;
    tl->scene_tree = wlr_scene_xdg_surface_create(&s->scene->tree, xdg->base);
    if (!tl->scene_tree) {
        free(tl);
        return;
    }

    xdg->base->data = tl->scene_tree;
    tl->scene_tree->node.data = tl;
    wlr_scene_node_set_position(&tl->scene_tree->node, 0, 0);

    wl_list_init(&tl->subsurfaces);
    wl_list_insert(&s->toplevels, &tl->link);

    tl->commit.notify = toplevel_commit;
    wl_signal_add(&xdg->base->surface->events.commit, &tl->commit);

    tl->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg->base->events.destroy, &tl->destroy);

    wlr_log(WLR_INFO, "New toplevel");
}

/*
 * toplevel.c - Toplevel and subsurface handlers (refactored)
 *
 * Handles XDG toplevel lifecycle, subsurface tracking,
 * and surface commit processing.
 *
 * Focus-related logic has been consolidated into focus_manager.c
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
#include "../types.h"  /* Includes focus_manager.h */
#include "../draw/draw.h"
#include "../p9/p9.h"

/* Forward declarations */
static void check_new_subsurfaces(struct toplevel *tl);

/* ============== Subsurface Tracking ============== */

static void subsurface_commit(struct wl_listener *l, void *d) {
    struct subsurface_track *st = wl_container_of(l, st, commit);
    (void)d;
    
    struct wlr_surface *surface = st->subsurface->surface;
    bool has_buffer = wlr_surface_has_buffer(surface);
    
    if (has_buffer && !st->mapped) {
        st->mapped = true;
        wlr_log(WLR_INFO, "Subsurface mapped: %p", surface);
        focus_pointer_recheck(&st->server->focus);
    } else if (!has_buffer && st->mapped) {
        st->mapped = false;
        wlr_log(WLR_INFO, "Subsurface unmapped: %p", surface);
        focus_pointer_recheck(&st->server->focus);
    }
    
    wlr_output_schedule_frame(st->server->output);
}

static void subsurface_destroy(struct wl_listener *l, void *d) {
    struct subsurface_track *st = wl_container_of(l, st, destroy);
    (void)d;
    
    wlr_log(WLR_INFO, "Subsurface destroyed: %p", st->subsurface->surface);
    
    wl_list_remove(&st->destroy.link);
    wl_list_remove(&st->commit.link);
    wl_list_remove(&st->link);
    free(st);
}

static bool is_subsurface_tracked(struct toplevel *tl, struct wlr_subsurface *subsurface) {
    struct subsurface_track *st;
    wl_list_for_each(st, &tl->subsurfaces, link) {
        if (st->subsurface == subsurface) {
            return true;
        }
    }
    return false;
}

static void track_subsurface(struct toplevel *tl, struct wlr_subsurface *subsurface) {
    wlr_log(WLR_INFO, "New subsurface: parent=%p surface=%p", 
            subsurface->parent, subsurface->surface);
    
    struct subsurface_track *st = calloc(1, sizeof(*st));
    if (!st) return;
    
    st->subsurface = subsurface;
    st->server = tl->server;
    st->toplevel = tl;
    st->mapped = false;
    
    st->destroy.notify = subsurface_destroy;
    wl_signal_add(&subsurface->events.destroy, &st->destroy);
    
    st->commit.notify = subsurface_commit;
    wl_signal_add(&subsurface->surface->events.commit, &st->commit);
    
    wl_list_insert(&tl->subsurfaces, &st->link);
    
    focus_pointer_recheck(&tl->server->focus);
}

static void check_new_subsurfaces(struct toplevel *tl) {
    struct wlr_surface *surface = tl->xdg->base->surface;
    struct wlr_subsurface *subsurface;
    
    wl_list_for_each(subsurface, &surface->current.subsurfaces_below, current.link) {
        if (!is_subsurface_tracked(tl, subsurface)) {
            track_subsurface(tl, subsurface);
        }
    }
    
    wl_list_for_each(subsurface, &surface->current.subsurfaces_above, current.link) {
        if (!is_subsurface_tracked(tl, subsurface)) {
            track_subsurface(tl, subsurface);
        }
    }
}

/* ============== Toplevel Handlers ============== */

static void toplevel_commit(struct wl_listener *l, void *d) { 
    struct toplevel *tl = wl_container_of(l, tl, commit);
    (void)d;
    
    struct server *s = tl->server;
    struct wlr_xdg_surface *xdg_surface = tl->xdg->base;
    struct wlr_surface *surface = xdg_surface->surface;
    
    if (xdg_surface->initial_commit) {
        /* Use logical dimensions for Wayland clients */
        int logical_w = focus_phys_to_logical(s->width, s->scale);
        int logical_h = focus_phys_to_logical(s->height, s->scale);
        
        wlr_xdg_toplevel_set_size(tl->xdg, logical_w, logical_h);
        wlr_xdg_toplevel_set_maximized(tl->xdg, true);
        wlr_xdg_toplevel_set_activated(tl->xdg, true);
        wlr_xdg_surface_schedule_configure(xdg_surface);
        
        tl->configured = true;
        wlr_log(WLR_INFO, "Initial commit: scheduled configure %dx%d", 
                logical_w, logical_h);
        return;
    }
    
    if (!surface->mapped) {
        return;
    }
    
    tl->commit_count++;
    bool has_buffer = wlr_surface_has_buffer(surface);
    
    if (tl->commit_count <= 10 || tl->commit_count % 60 == 0) {
        wlr_log(WLR_INFO, "Toplevel %p commit #%d has_buffer=%d mapped=%d", 
                (void*)tl, tl->commit_count, has_buffer, tl->mapped);
    }
    
    /* Track map/unmap state changes */
    if (has_buffer && !tl->mapped) {
        tl->mapped = true;
        wlr_log(WLR_INFO, "Toplevel MAPPED!");
        
        /* Notify focus manager of map */
        focus_on_surface_map(&s->focus, surface, true);
        
        /* Check if cursor is over this surface */
        double sx, sy;
        struct wlr_surface *under = focus_surface_at_cursor(&s->focus, &sx, &sy);
        if (under == surface) {
            focus_pointer_set(&s->focus, surface, sx, sy, FOCUS_REASON_SURFACE_MAP);
        }
        
    } else if (!has_buffer && tl->mapped) {
        tl->mapped = false;
        wlr_log(WLR_DEBUG, "Toplevel unmapped");
        
        /* Notify focus manager of unmap */
        focus_on_surface_unmap(&s->focus, surface);
    }
    
    check_new_subsurfaces(tl);
    focus_pointer_recheck(&s->focus);
    wlr_output_schedule_frame(s->output);
}

static void toplevel_destroy(struct wl_listener *l, void *d) {
    struct toplevel *tl = wl_container_of(l, tl, destroy);
    struct server *s = tl->server;
    (void)d;
    
    wlr_log(WLR_INFO, "Toplevel destroyed: surface=%p", (void*)tl->surface);
    
    /* Notify focus manager before cleanup */
    focus_on_surface_destroy(&s->focus, tl->surface);
    
    /* Clean up subsurface tracking */
    struct subsurface_track *st, *tmp;
    wl_list_for_each_safe(st, tmp, &tl->subsurfaces, link) {
        wl_list_remove(&st->destroy.link);
        wl_list_remove(&st->commit.link);
        wl_list_remove(&st->link);
        free(st);
    }
    
    /* Remove listeners and free */
    wl_list_remove(&tl->commit.link);
    wl_list_remove(&tl->destroy.link);
    wl_list_remove(&tl->link);
    free(tl);
    
    /* Exit when last toplevel is destroyed */
    if (s->had_toplevel && wl_list_empty(&s->toplevels)) {
        wlr_log(WLR_INFO, "Last toplevel destroyed - initiating graceful shutdown");
        
        /* Stop send thread before touching 9P connection */
        pthread_mutex_lock(&s->send_lock);
        s->running = 0;
        pthread_cond_signal(&s->send_cond);
        pthread_mutex_unlock(&s->send_lock);
        
        if (s->send_thread) {
            wlr_log(WLR_INFO, "Waiting for send thread to finish...");
            pthread_join(s->send_thread, NULL);
            s->send_thread = 0;
        }
        
        if (s->wctl_thread) {
            pthread_cancel(s->wctl_thread);
            pthread_join(s->wctl_thread, NULL);
            s->wctl_thread = 0;
        }
        
        wlr_log(WLR_INFO, "Deleting rio window...");
        delete_rio_window(&s->p9_draw);
        
        wlr_log(WLR_INFO, "Disconnecting from 9P server...");
        p9_disconnect(&s->p9_draw);
        
        wlr_log(WLR_INFO, "Shutdown complete, exiting");
        exit(0);
    }
}

void new_toplevel(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg = d;
    
    wlr_log(WLR_INFO, "New XDG toplevel created");
    
    s->has_toplevel = 1;
    s->had_toplevel = 1;
    
    struct toplevel *tl = calloc(1, sizeof(*tl));
    if (!tl) {
        wlr_log(WLR_ERROR, "Failed to allocate toplevel");
        return;
    }
    
    tl->xdg = xdg;
    tl->server = s;
    tl->surface = xdg->base->surface;
    tl->commit_count = 0;
    tl->mapped = false;
    tl->scene_tree = wlr_scene_xdg_surface_create(&s->scene->tree, xdg->base);
    
    if (!tl->scene_tree) {
        wlr_log(WLR_ERROR, "Failed to create scene tree for XDG surface");
        free(tl);
        return;
    }
    
    xdg->base->data = tl->scene_tree;
    tl->scene_tree->node.data = tl;
    wlr_scene_node_set_position(&tl->scene_tree->node, 0, 0);
    
    wl_list_init(&tl->subsurfaces);
    wl_list_init(&tl->link);
    
    /* Add to toplevels list */
    wl_list_insert(&s->toplevels, &tl->link);
    
    /* Setup listeners */
    tl->commit.notify = toplevel_commit;
    wl_signal_add(&xdg->base->surface->events.commit, &tl->commit);
    
    tl->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg->base->events.destroy, &tl->destroy);
    
    wlr_log(WLR_INFO, "XDG surface scene tree created at (0,0)");
}

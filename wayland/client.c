/*
 * client.c - Client handling, decoration, and cleanup
 *
 * Handles XDG decorations (server-side) and server resource cleanup.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/util/log.h>

#include "client.h"
#include "../draw/draw.h"
#include "../p9/p9.h"

/* ============== Decoration Handling ============== */

struct decoration_data {
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener destroy;
    struct wl_listener request_mode;
    struct wl_listener surface_commit;
    bool mode_set;
};

static void decoration_handle_destroy(struct wl_listener *listener, void *data) {
    struct decoration_data *dd = wl_container_of(listener, dd, destroy);
    (void)data;
    wl_list_remove(&dd->destroy.link);
    wl_list_remove(&dd->request_mode.link);
    if (dd->surface_commit.link.next) {
        wl_list_remove(&dd->surface_commit.link);
    }
    free(dd);
}

static void decoration_set_mode_if_ready(struct decoration_data *dd) {
    struct wlr_xdg_toplevel *toplevel = dd->decoration->toplevel;
    
    if (!toplevel || !toplevel->base || !toplevel->base->initialized) {
        wlr_log(WLR_DEBUG, "Decoration: surface not initialized yet, deferring");
        return;
    }
    
    if (dd->mode_set) {
        return;
    }
    
    wlr_log(WLR_INFO, "Decoration mode set to server-side");
    wlr_xdg_toplevel_decoration_v1_set_mode(dd->decoration, 
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    dd->mode_set = true;
    
    if (dd->surface_commit.link.next) {
        wl_list_remove(&dd->surface_commit.link);
        dd->surface_commit.link.next = NULL;
    }
}

static void decoration_handle_surface_commit(struct wl_listener *listener, void *data) {
    struct decoration_data *dd = wl_container_of(listener, dd, surface_commit);
    (void)data;
    decoration_set_mode_if_ready(dd);
}

static void decoration_handle_request_mode(struct wl_listener *listener, void *data) {
    struct decoration_data *dd = wl_container_of(listener, dd, request_mode);
    (void)data;
    
    decoration_set_mode_if_ready(dd);
    
    if (!dd->mode_set && dd->decoration->toplevel && 
        dd->decoration->toplevel->base && dd->decoration->toplevel->base->surface) {
        if (!dd->surface_commit.link.next) {
            dd->surface_commit.notify = decoration_handle_surface_commit;
            wl_signal_add(&dd->decoration->toplevel->base->surface->events.commit, 
                          &dd->surface_commit);
        }
    }
}

void handle_new_decoration(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, new_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
    (void)s;
    
    wlr_log(WLR_INFO, "New decoration object created");
    
    struct decoration_data *dd = calloc(1, sizeof(*dd));
    if (!dd) {
        wlr_log(WLR_ERROR, "Failed to allocate decoration_data");
        return;
    }
    
    dd->decoration = decoration;
    dd->mode_set = false;
    dd->surface_commit.link.next = NULL;
    
    dd->destroy.notify = decoration_handle_destroy;
    wl_signal_add(&decoration->events.destroy, &dd->destroy);
    
    dd->request_mode.notify = decoration_handle_request_mode;
    wl_signal_add(&decoration->events.request_mode, &dd->request_mode);
}

/* ============== Server Cleanup ============== */

void server_cleanup(struct server *s) {
    s->running = 0;
    
    pthread_mutex_lock(&s->send_lock);
    pthread_cond_signal(&s->send_cond);
    pthread_mutex_unlock(&s->send_lock);
    
    if (s->mouse_thread) pthread_join(s->mouse_thread, NULL);
    if (s->kbd_thread)   pthread_join(s->kbd_thread, NULL);
    if (s->send_thread)  pthread_join(s->send_thread, NULL);
    
    wlr_keyboard_finish(&s->virtual_kb);
    
    /* Cleanup focus manager */
    focus_manager_cleanup(&s->focus);
    
    free(s->framebuf);
    free(s->prev_framebuf);
    free(s->send_buf[0]);
    free(s->send_buf[1]);
    
    pthread_mutex_destroy(&s->send_lock);
    pthread_cond_destroy(&s->send_cond);
    
    p9_disconnect(&s->p9_draw);
    p9_disconnect(&s->p9_mouse);
    p9_disconnect(&s->p9_kbd);
    p9_disconnect(&s->p9_wctl);
    p9_disconnect(&s->p9_snarf);
    
    close(s->input_queue.pipe_fd[0]);
    close(s->input_queue.pipe_fd[1]);
    pthread_mutex_destroy(&s->input_queue.lock);
    
    free(s->tls_cert_file);
    free(s->tls_fingerprint);
}

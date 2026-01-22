/*
 * window.h - Window instance management
 *
 * Handles creation, destruction, and management of rio windows.
 * Each window_instance represents one rio window with its own
 * 9P connections, framebuffers, and associated Wayland toplevels.
 */

#ifndef P9WL_WINDOW_H
#define P9WL_WINDOW_H

#include "../types.h"

/*
 * Create a new window instance.
 * This creates a new rio window with exportfs and connects to it.
 * 
 * Returns: newly allocated window_instance, or NULL on failure
 */
struct window_instance *window_create(struct server *s, struct wl_client *owner);

/*
 * Initialize the primary window (the first window created at startup).
 * This is called with the initial 9P connections from main().
 * 
 * Returns: 0 on success, -1 on failure
 */
int window_init_primary(struct server *s, struct p9conn *p9_draw, 
                        struct p9conn *p9_mouse, struct p9conn *p9_wctl);

/*
 * Destroy a window instance.
 * Cleans up rio window, closes 9P connections, frees memory.
 */
void window_destroy(struct window_instance *win);

/*
 * Find the window instance for a given client.
 * Returns the client's assigned window, or primary_window if none assigned.
 */
struct window_instance *window_for_client(struct server *s, struct wl_client *client);

/*
 * Assign a client to a window.
 */
void window_assign_client(struct window_instance *win, struct wl_client *client);

/*
 * Start input threads for a window (mouse, wctl).
 * Keyboard thread is shared and started separately.
 */
int window_start_threads(struct window_instance *win);

/*
 * Stop input threads for a window.
 */
void window_stop_threads(struct window_instance *win);

/*
 * Initialize draw device for a window.
 * Returns: 0 on success, -1 on failure
 */
int window_init_draw(struct window_instance *win);

/*
 * Allocate framebuffers for a window.
 * Returns: 0 on success, -1 on failure
 */
int window_alloc_framebuffers(struct window_instance *win);

/*
 * Initialize scene graph for a window.
 * Creates a virtual output and scene for independent rendering.
 * Returns: 0 on success, -1 on failure
 */
int window_init_scene(struct window_instance *win);

/*
 * Destroy scene graph for a window.
 */
void window_destroy_scene(struct window_instance *win);

/*
 * Handle window resize.
 */
void window_handle_resize(struct window_instance *win, int new_width, int new_height);

/*
 * Render frame for a window.
 */
void window_render_frame(struct window_instance *win);

/*
 * Check if window has any toplevels.
 */
static inline int window_has_toplevels(struct window_instance *win) {
    return !wl_list_empty(&win->toplevels);
}

#endif /* P9WL_WINDOW_H */

/*
 * client.h - Client decoration and server cleanup
 *
 * This module handles two responsibilities:
 *
 * XDG Decoration:
 *
 *   Wayland clients can request either client-side or server-side window
 *   decorations. The p9wl compositor forces server-side decorations so
 *   that it can draw window borders via the Plan 9 draw protocol.
 *
 *   When a client creates an xdg_toplevel_decoration object, the
 *   handle_new_decoration() handler sets the mode to server-side.
 *   This may need to be deferred until the surface is initialized.
 *
 *   Internal decoration_data struct tracks:
 *     - decoration: the wlr_xdg_toplevel_decoration_v1 object
 *     - destroy/request_mode/surface_commit: Wayland listeners
 *     - mode_set: flag to avoid setting mode multiple times
 *
 * Server Cleanup:
 *
 *   server_cleanup() performs orderly shutdown of the compositor:
 *
 *     1. Signal threads to stop (s->running = 0)
 *     2. Wake send thread via condition variable
 *     3. Join all worker threads (mouse, keyboard, send)
 *     4. Clean up virtual keyboard via wlr_keyboard_finish()
 *     5. Log focus manager statistics via focus_manager_cleanup()
 *     6. Free framebuffers (framebuf, prev_framebuf, send_buf[0/1])
 *     7. Destroy synchronization primitives (mutex, condvar)
 *     8. Disconnect all 9P connections (draw, mouse, kbd, wctl, snarf)
 *     9. Close input queue pipe and destroy its mutex
 *     10. Free TLS configuration strings
 *
 * Usage:
 *
 *   Wire up the decoration handler during server initialization:
 *
 *     server->new_decoration.notify = handle_new_decoration;
 *     wl_signal_add(&decoration_mgr->events.new_toplevel_decoration,
 *                   &server->new_decoration);
 *
 *   Call server_cleanup() during shutdown (or let toplevel_destroy()
 *   call exit() which doesn't require explicit cleanup).
 */

#ifndef P9WL_CLIENT_H
#define P9WL_CLIENT_H

#include <stdbool.h>
#include "../types.h"

/* ============== Decoration Handling ============== */

/*
 * Handle new XDG toplevel decoration request.
 *
 * Forces server-side decoration mode. If the underlying XDG surface is
 * not yet initialized (xdg->base->initialized is false), defers the mode
 * setting by adding a surface commit listener that will retry on each
 * commit until the surface is ready.
 *
 * Allocates internal decoration_data struct that is automatically freed
 * when the decoration is destroyed.
 *
 * listener: wl_listener from server->new_decoration
 * data:     wlr_xdg_toplevel_decoration_v1 pointer
 */
void handle_new_decoration(struct wl_listener *listener, void *data);

/* ============== Server Cleanup ============== */

/*
 * Clean up server resources during shutdown.
 *
 * Stops all worker threads, frees allocated memory, destroys
 * synchronization primitives, and disconnects 9P connections.
 *
 * Thread shutdown sequence:
 *   1. Sets s->running = 0
 *   2. Signals send_cond to wake send thread
 *   3. Joins mouse_thread, kbd_thread, send_thread
 *
 * This is typically not called explicitly since the compositor exits
 * via exit() when the last toplevel is destroyed in toplevel_destroy().
 *
 * s: server state to clean up
 */
void server_cleanup(struct server *s);

#endif /* P9WL_CLIENT_H */

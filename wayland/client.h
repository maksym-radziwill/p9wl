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
 * Server Cleanup:
 *
 *   server_cleanup() performs orderly shutdown of the compositor:
 *
 *     1. Signal threads to stop
 *     2. Join all worker threads (mouse, keyboard, wctl, send)
 *     3. Clean up virtual keyboard
 *     4. Clean up focus manager
 *     5. Free framebuffers
 *     6. Destroy synchronization primitives
 *     7. Disconnect 9P connections
 *     8. Close input queue pipe
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

/*
 * Handle new XDG toplevel decoration request.
 *
 * Forces server-side decoration mode. If the underlying XDG surface is
 * not yet initialized, defers the mode setting until the surface commits.
 */
void handle_new_decoration(struct wl_listener *listener, void *data);

/*
 * Clean up server resources during shutdown.
 *
 * Stops all worker threads, frees allocated memory, destroys
 * synchronization primitives, and disconnects 9P connections.
 *
 * This is typically not called explicitly since the compositor exits
 * via exit() when the last toplevel is destroyed.
 */
void server_cleanup(struct server *s);

#endif /* P9WL_CLIENT_H */

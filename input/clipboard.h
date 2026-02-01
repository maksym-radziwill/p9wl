/*
 * clipboard.h - Wayland clipboard <-> Plan 9 /dev/snarf integration
 *
 * Provides bidirectional clipboard synchronization:
 *   - Wayland client copies (Ctrl+C) -> writes to /dev/snarf
 *   - Wayland client pastes (Ctrl+V) -> reads from /dev/snarf
 *
 * Design:
 *
 *   All 9P I/O is performed asynchronously to avoid blocking the Wayland
 *   event loop. Snarf is treated as the single source of truth for
 *   clipboard contents.
 *
 *   Supported MIME types for text:
 *     - text/plain;charset=utf-8
 *     - text/plain
 *     - UTF8_STRING, STRING, TEXT (X11 compatibility)
 *
 *   Maximum clipboard size: 1MB (SNARF_MAX_SIZE)
 *
 * Wayland -> Snarf (Copy):
 *
 *   When a Wayland client sets the selection (copies):
 *     1. on_wayland_copy() handler is called
 *     2. Client becomes selection owner (protocol requirement)
 *     3. Async read via Wayland event loop fd
 *     4. Data written to /dev/snarf via p9_write_file()
 *     5. Compositor reclaims selection ownership
 *     6. Future pastes (even Wayland-to-Wayland) go through snarf
 *
 * Snarf -> Wayland (Paste):
 *
 *   The compositor registers as selection owner, so paste requests
 *   come to us:
 *     1. Client requests paste via wlr_data_source_send()
 *     2. snarf_to_wayland_send() spawns detached thread
 *     3. Thread reads from /dev/snarf (blocking OK in thread)
 *     4. Thread writes to client fd and closes it
 *
 *   This async approach prevents blocking the compositor on 9P I/O.
 *
 * Primary Selection:
 *
 *   Primary selection (highlight-to-copy, middle-click paste) is NOT
 *   synced to snarf. This is intentional - primary selection changes
 *   on every text highlight, which would overwrite the clipboard
 *   unexpectedly. Only explicit Ctrl+C copies go to snarf.
 *
 * Usage:
 *
 *   Initialize during server setup (after seat and p9_snarf are ready):
 *
 *     if (clipboard_init(server) < 0) {
 *         // handle error
 *     }
 *
 *   Clean up during shutdown:
 *
 *     clipboard_cleanup(server);
 */

#ifndef P9WL_CLIPBOARD_H
#define P9WL_CLIPBOARD_H

struct server;

/* ============== Initialization ============== */

/*
 * Initialize clipboard handling.
 *
 * Sets up listeners for Wayland selection events:
 *   - request_set_selection (Ctrl+C copy)
 *   - request_set_primary_selection (highlight copy, not synced)
 *
 * Registers as initial selection owner so paste requests read from
 * /dev/snarf. Uses p9_snarf connection for 9P operations.
 *
 * s: server instance (must have seat and p9_snarf initialized)
 *
 * Returns 0 on success, -1 on failure.
 */
int clipboard_init(struct server *s);

/*
 * Clean up clipboard resources.
 *
 * Removes Wayland event listeners:
 *   - wayland_to_snarf (copy handler)
 *   - wayland_to_snarf_primary (primary selection handler)
 *
 * Should be called during server shutdown. Any in-flight async
 * operations (paste threads) will complete independently.
 *
 * s: server instance
 */
void clipboard_cleanup(struct server *s);

#endif /* P9WL_CLIPBOARD_H */

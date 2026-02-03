/*
 * clipboard.h - Wayland clipboard <-> Plan 9 /dev/snarf bridge
 *
 * This module handles:
 *   - Copy: capturing Wayland client selection data and writing to /dev/snarf
 *   - Paste: reading /dev/snarf and providing data to Wayland clients
 *   - Primary selection: pass-through without snarf sync
 *
 * Design:
 *
 *   Snarf is treated as the single source of truth for clipboard
 *   contents. After a Wayland client copies, the compositor reclaims
 *   selection ownership so all future pastes (even Wayland-to-Wayland)
 *   go through /dev/snarf.
 *
 *   Supported MIME types for text:
 *     - text/plain, text/plain;charset=utf-8
 *     - UTF8_STRING, STRING, TEXT (X11 compatibility)
 *
 *   Maximum clipboard size: 1MB (SNARF_MAX)
 *
 * Copy (Wayland → Snarf):
 *
 *   When a Wayland client sets the selection:
 *     1. on_copy() lets client become selection owner (protocol requirement)
 *     2. Data is read asynchronously via event loop fd (copy_readable())
 *     3. On EOF, data is written to /dev/snarf via p9_write_file()
 *     4. Compositor reclaims selection ownership via reclaim_selection()
 *
 * Paste (Snarf → Wayland):
 *
 *   The compositor registers as selection owner, so paste requests
 *   come to snarf_send():
 *     1. A detached thread is spawned (paste_thread())
 *     2. Thread reads from /dev/snarf via p9_read_file() (blocking OK)
 *     3. Thread writes to client fd and closes it
 *
 *   This prevents blocking the compositor on 9P I/O.
 *
 * Primary Selection:
 *
 *   Primary selection (highlight-to-copy, middle-click paste) is passed
 *   through to Wayland without syncing to snarf. Primary selection
 *   changes on every text highlight, which would overwrite the clipboard
 *   unexpectedly. Only explicit copies go to snarf.
 *
 * Usage:
 *
 *   Initialize during server setup (after seat and p9_snarf are ready):
 *
 *     clipboard_init(server);
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
 *   - request_set_selection (copy)
 *   - request_set_primary_selection (highlight copy, not synced)
 *
 * Registers as initial selection owner so paste requests read
 * from /dev/snarf.
 *
 * s: server instance (must have seat and p9_snarf initialized)
 *
 * Returns 0 on success.
 */
int clipboard_init(struct server *s);

/*
 * Clean up clipboard resources.
 *
 * Removes Wayland event listeners for copy and primary selection.
 * Any in-flight paste threads will complete independently.
 *
 * s: server instance
 */
void clipboard_cleanup(struct server *s);

#endif /* P9WL_CLIPBOARD_H */
